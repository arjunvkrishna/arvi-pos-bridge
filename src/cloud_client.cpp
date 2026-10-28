#include "cloud_client.h"
#include "config.h"
#include "root_ca.h"
#include "system_state.h"
#include "printer_client.h"
#include "print_queue.h"
#include "rgb_led_manager.h"
#include <WiFi.h>
#include <time.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include <new>

CloudClient g_cloudClient;

// ------------------------------------------------------------------------------
// Helpers: PSRAM-backed response buffer, PSRAM allocator for ArduinoJson
// ------------------------------------------------------------------------------
static void* psramAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static void* psramRealloc(void* old, size_t n) {
    void* p = heap_caps_realloc(old, n, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_realloc(old, n, MALLOC_CAP_8BIT);
}

// Grows geometrically but never beyond a hard cap, so a hostile/buggy server cannot exhaust memory.
class CappedBuffer : public Stream {
public:
    explicit CappedBuffer(size_t cap) : m_cap(cap) {}
    ~CappedBuffer() override { if (m_buf) heap_caps_free(m_buf); }

    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* p, size_t n) override {
        if (m_len + n > m_cap) { m_overflow = true; return 0; }
        if (!ensure(m_len + n + 1)) { m_overflow = true; return 0; }
        memcpy(m_buf + m_len, p, n);
        m_len += n;
        m_buf[m_len] = 0;
        return n;
    }
    void hint(size_t n) { if (n > 0 && n <= m_cap) ensure(n + 1); }
    char* data() { return m_buf; }
    size_t size() const { return m_len; }
    size_t cap() const { return m_cap; }
    bool overflowed() const { return m_overflow; }

    // Write-only sink: HTTPClient::writeToStream() needs a Stream
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

private:
    bool ensure(size_t need) {
        if (need <= m_alloc) return true;
        size_t newAlloc = max(need, m_alloc ? m_alloc * 2 : (size_t)2048);
        if (newAlloc > m_cap + 1) newAlloc = m_cap + 1;
        if (newAlloc < need) return false;
        char* p = (char*)psramRealloc(m_buf, newAlloc);
        if (!p) return false;
        m_buf = p;
        m_alloc = newAlloc;
        return true;
    }
    char* m_buf = nullptr;
    size_t m_len = 0, m_alloc = 0, m_cap;
    bool m_overflow = false;
};

class NullPrint : public Stream {
public:
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    size_t write(uint8_t) override { return 1; }
    size_t write(const uint8_t*, size_t n) override { return n; }
};

struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return psramAlloc(n); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return psramRealloc(p, n); }
};
static PsramJsonAllocator g_jsonAllocator;

static const int HTTP_ERR_TOO_LARGE = -100;

static bool validJobId(const char* id) {
    size_t n = strlen(id);
    if (n == 0 || n > MAX_JOB_ID_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
    }
    return true;
}

// ------------------------------------------------------------------------------
// Setup
// ------------------------------------------------------------------------------
CloudClient::CloudClient() {
    m_apiMutex = xSemaphoreCreateMutex();
}

void CloudClient::begin() {
    bool secure = strncmp(BACKEND_BASE_URL, "https://", 8) == 0;
    m_poll.secure = secure;
    m_api.secure = secure;
    if (secure) {
        m_poll.tls.setCACert(NERAM_ROOT_CA_PEM);
        m_api.tls.setCACert(NERAM_ROOT_CA_PEM);
        m_poll.tls.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
        m_api.tls.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
    } else {
        Serial.println("[CLOUD] WARNING: backend URL is plain HTTP (ALLOW_INSECURE_HTTP build). Lab use only!");
    }

    if (strlen(BRIDGE_ID_OVERRIDE) > 0) {
        m_bridgeId = BRIDGE_ID_OVERRIDE;
    } else {
        char buf[40];
        snprintf(buf, sizeof(buf), "%s-%06X", BRIDGE_DEVICE_ID_PREFIX, (unsigned)(ESP.getEfuseMac() >> 24) & 0xFFFFFF);
        m_bridgeId = buf;
    }
    Serial.printf("[CLOUD] Bridge ID: %s | Backend: %s\n", m_bridgeId.c_str(), BACKEND_BASE_URL);
}

bool CloudClient::ensureTime(uint32_t waitMs) {
    if (time(nullptr) > (time_t)NTP_VALID_AFTER_EPOCH) return true;
    if (!m_ntpStarted) {
        configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
        m_ntpStarted = true;
        Serial.println("[NTP] Syncing clock (required for TLS certificate validation)...");
    }
    uint32_t t0 = millis();
    while (millis() - t0 < waitMs) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (time(nullptr) > (time_t)NTP_VALID_AFTER_EPOCH) {
            Serial.println("[NTP] Clock synchronized");
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------
// HTTP plumbing (persistent keep-alive connection per channel)
// ------------------------------------------------------------------------------
int CloudClient::request(Channel& ch, bool post, const String& path, const String* body, uint32_t timeoutMs, CappedBuffer* out) {
    String url = String(BACKEND_BASE_URL) + path;

    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = ch.client().connected();
        if (!ch.http.begin(ch.client(), url)) return -1;
        ch.http.setReuse(true);
        ch.http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
        ch.http.setTimeout(timeoutMs);
        ch.http.addHeader("X-Bridge-Token", BACKEND_AUTH_TOKEN);
        ch.http.addHeader("X-Tenant-Code", BACKEND_TENANT_CODE); // which cafe this bridge prints for
#ifdef BACKEND_OUTLET_CODE
        ch.http.addHeader("X-Outlet-Code", BACKEND_OUTLET_CODE); // which branch: only that branch's tickets are handed out
#endif
        if (post) ch.http.addHeader("Content-Type", "application/json");

        int code = post ? ch.http.POST(body ? *body : String("{}")) : ch.http.GET();

        // A kept-alive socket the server already closed fails on first use: reconnect once, transparently.
        if (code < 0 && reused && attempt == 0) {
            ch.http.end();
            ch.client().stop();
            continue;
        }

        if (code == 200) {
            if (out) {
                int sz = ch.http.getSize();
                if (sz > 0 && (size_t)sz > out->cap()) {
                    code = HTTP_ERR_TOO_LARGE;
                    ch.client().stop();
                } else {
                    out->hint(sz > 0 ? (size_t)sz : 0);
                    int n = ch.http.writeToStream(out);
                    if (n < 0 || out->overflowed()) {
                        code = out->overflowed() ? HTTP_ERR_TOO_LARGE : -2;
                        ch.client().stop();
                    }
                }
            } else {
                NullPrint sink;
                ch.http.writeToStream(&sink);     // Drain the body so the keep-alive socket stays in sync
            }
        } else if (code != 204) {
            ch.client().stop();                       // Unread error body would desync the next response
        }
        ch.http.end();
        return code;
    }
    return -1;
}

int CloudClient::apiRequest(bool post, const String& path, const String* body, CappedBuffer* out) {
    xSemaphoreTake(m_apiMutex, portMAX_DELAY);
    int code = request(m_api, post, path, body, API_HTTP_TIMEOUT_MS, out);
    xSemaphoreGive(m_apiMutex);
    return code;
}

bool CloudClient::postWithRetry(const String& path, const String& body, int tries) {
    for (int i = 0; i < tries; i++) {
        int code = apiRequest(true, path, &body);
        if (code == 200 || code == 204) return true;
        bool transient = code < 0 || code == 408 || code == 429 || code >= 500;
        if (!transient) {
            Serial.printf("[CLOUD] POST %s rejected: HTTP %d\n", path.c_str(), code);
            return false;
        }
        if (i + 1 < tries) vTaskDelay(pdMS_TO_TICKS(REPORT_BACKOFF_MS << i));
    }
    return false;
}

String CloudClient::jobPath(const char* jobId, const char* action) const {
    return String("/api/printer/bridge/jobs/") + jobId + "/" + action;
}

// ------------------------------------------------------------------------------
// Config sync & heartbeat
// ------------------------------------------------------------------------------
bool CloudClient::fetchPrinterConfig() {
    CappedBuffer body(16 * 1024);
    int code = apiRequest(false, "/api/printer/bridge/config", nullptr, &body);
    if (code != 200) {
        Serial.printf("[CLOUD] Printer config fetch failed: HTTP %d\n", code);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body.data(), body.size());
    if (err) {
        Serial.printf("[CLOUD] Printer config JSON error: %s\n", err.c_str());
        return false;
    }

    String billHost = doc["billHost"] | "";
    int billPort = doc["billPort"] | 0;
    if (billHost.length() == 0) billHost = doc["host"] | "";
    if (billPort <= 0) billPort = doc["port"] | 9100;

    String kotHost = doc["kotHost"] | "";
    int kotPort = doc["kotPort"] | 0;
    if (kotHost.length() == 0) kotHost = billHost;
    if (kotPort <= 0) kotPort = 9100;

    String juiceHost = doc["juiceKotHost"] | "";
    int juicePort = doc["juiceKotPort"] | 0;
    if (juicePort <= 0) juicePort = 9100;

    // Never accept a target the print path would refuse (public IPs, odd ports, ...)
    String why;
    if (billHost.length() > 0 && !g_printerClient.isAllowedTarget(billHost, billPort, why)) {
        Serial.printf("[CLOUD] Ignoring bill printer %s:%d from server: %s\n", billHost.c_str(), billPort, why.c_str());
        billHost = "";
    }
    if (kotHost.length() > 0 && !g_printerClient.isAllowedTarget(kotHost, kotPort, why)) {
        Serial.printf("[CLOUD] Ignoring KOT printer %s:%d from server: %s\n", kotHost.c_str(), kotPort, why.c_str());
        kotHost = "";
    }

    if (juiceHost.length() > 0 && !g_printerClient.isAllowedTarget(juiceHost, juicePort, why)) {
        Serial.printf("[CLOUD] Ignoring Juice KOT printer %s:%d from server: %s\n", juiceHost.c_str(), juicePort, why.c_str());
        juiceHost = "";
    }

    bool changed = false;
    {
        StateLock lock;
        if (lock.ok()) {
            if (billHost.length() > 0 && (g_systemState.billHost != billHost || g_systemState.billPort != billPort)) {
                Serial.printf("[CLOUD] Bill printer: %s:%d (was %s:%d)\n", billHost.c_str(), billPort,
                              g_systemState.billHost.c_str(), g_systemState.billPort);
                g_systemState.billHost = billHost;
                g_systemState.billPort = billPort;
                changed = true;
            }
            if (kotHost.length() > 0 && (g_systemState.kotHost != kotHost || g_systemState.kotPort != kotPort)) {
                Serial.printf("[CLOUD] KOT printer: %s:%d (was %s:%d)\n", kotHost.c_str(), kotPort,
                              g_systemState.kotHost.c_str(), g_systemState.kotPort);
                g_systemState.kotHost = kotHost;
                g_systemState.kotPort = kotPort;
                changed = true;
            }
            if (juiceHost.length() > 0 && (g_systemState.juiceKotHost != juiceHost || g_systemState.juiceKotPort != juicePort)) {
                Serial.printf("[CLOUD] Juice KOT printer: %s:%d (was %s:%d)\n", juiceHost.c_str(), juicePort,
                              g_systemState.juiceKotHost.c_str(), g_systemState.juiceKotPort);
                g_systemState.juiceKotHost = juiceHost;
                g_systemState.juiceKotPort = juicePort;
                changed = true;
            }
        }
    }
    if (changed) {
        g_printerClient.checkPrinters(g_printQueue.isBusy(PRINTER_BILL), g_printQueue.isBusy(PRINTER_KOT), g_printQueue.isBusy(PRINTER_JUICE));
    }
    return true;
}

bool CloudClient::sendHeartbeat() {
    updateSystemHardwareStats();

    SystemState snap;
    {
        StateLock lock;
        if (!lock.ok()) return false;
        snap = g_systemState;
    }

    JsonDocument doc;
    doc["bridgeId"] = m_bridgeId;
    doc["version"] = FIRMWARE_VERSION;
    doc["hardwareModel"] = HARDWARE_MODEL;
    doc["ipAddress"] = snap.wifiIp;
    doc["freeHeap"] = (long)snap.freeHeapBytes;
    doc["freePsram"] = (long)snap.freePsramBytes;
    doc["uptimeSeconds"] = (long)snap.uptimeSeconds;
    doc["billPrinterHost"] = snap.billHost;
    doc["billPrinterPort"] = snap.billPort;
    doc["billPrinterReachable"] = snap.billReachable;
    doc["billPrinterLatencyMs"] = snap.billLatencyMs;
    doc["kotPrinterHost"] = snap.kotHost;
    doc["kotPrinterPort"] = snap.kotPort;
    doc["kotPrinterReachable"] = snap.kotReachable;
    doc["kotPrinterLatencyMs"] = snap.kotLatencyMs;
    doc["juiceKotPrinterHost"] = snap.juiceKotHost;
    doc["juiceKotPrinterPort"] = snap.juiceKotPort;
    doc["juiceKotPrinterReachable"] = snap.juiceKotReachable;
    doc["juiceKotPrinterLatencyMs"] = snap.juiceKotLatencyMs;
    doc["queueCount"] = g_printQueue.pendingCount();
    if (snap.hasActiveAlert) {
        doc["lastError"] = snap.activeAlertTitle + ": " + snap.activeAlertMessage;
    } else if (snap.lastEspError.length() > 0 && snap.lastEspError != "None") {
        doc["lastError"] = snap.lastEspError;
    } else {
        doc["lastError"] = nullptr;
    }

    String reqBody;
    serializeJson(doc, reqBody);

    int code = apiRequest(true, "/api/printer/bridge/heartbeat", &reqBody);
    bool ok = (code == 200 || code == 204);
    if (!ok) {
        StateLock lock;
        if (lock.ok()) {
            g_systemState.lastHttpCode = code;
            g_systemState.lastServerError = "Heartbeat failed (" + String(code) + ")";
        }
    }
    return ok;
}

// ------------------------------------------------------------------------------
// Polling
// ------------------------------------------------------------------------------
void CloudClient::recordPollOk(uint32_t latencyMs) {
    {
        StateLock lock;
        if (lock.ok()) {
            g_systemState.serverConnected = true;
            g_systemState.lastHttpCode = 200;
            g_systemState.lastServerError = "None";
            g_systemState.lastServerLatencyMs = latencyMs;
            g_systemState.successfulPolls++;
        }
    }
    clearAlertPrefix("AUTH ERROR");
}

void CloudClient::recordPollFailure(int code) {
    String errText;
    if (code == HTTP_ERR_TOO_LARGE) errText = "Response too large";
    else if (code < 0) errText = HTTPClient::errorToString(code);
    else errText = "HTTP " + String(code);

    {
        StateLock lock;
        if (lock.ok()) {
            g_systemState.serverConnected = false;
            g_systemState.lastHttpCode = code;
            g_systemState.lastServerError = errText;
            g_systemState.failedPolls++;
        }
    }
    if (code == 401) setAlert("AUTH ERROR 401", "Invalid Bridge Token");
}

int CloudClient::pollOnce() {
    String path = "/api/printer/bridge/poll?bridgeId=" + m_bridgeId + "&timeout=" + String(LONG_POLL_TIMEOUT_SEC);
    CappedBuffer body(MAX_RESPONSE_BYTES);

    uint32_t startMs = millis();
    int code = request(m_poll, false, path, nullptr, POLL_HTTP_TIMEOUT_MS, &body);
    uint32_t latency = millis() - startMs;

    if (code == 204) {                 // Long-poll window elapsed, no jobs
        recordPollOk(latency);
        return 0;
    }
    if (code != 200) {
        recordPollFailure(code);
        return -1;
    }
    recordPollOk(latency);
    return handlePollBody(body);
}

int CloudClient::handlePollBody(CappedBuffer& body) {
    if (body.size() == 0) return 0;

    // In-situ parse: strings point into the PSRAM buffer, so the base64 payload is never copied.
    JsonDocument doc(&g_jsonAllocator);
    DeserializationError err = deserializeJson(doc, body.data(), body.size());
    if (err) {
        Serial.printf("[CLOUD] Poll JSON error: %s\n", err.c_str());
        return 0;
    }

    int accepted = 0;
    if (doc.is<JsonArray>()) {
        for (JsonObject job : doc.as<JsonArray>()) {
            if (handleJob(job)) accepted++;
        }
    } else if (doc.is<JsonObject>()) {
        JsonObject obj = doc.as<JsonObject>();
        if (!obj["jobId"].isNull() || !obj["id"].isNull()) {
            if (handleJob(obj)) accepted++;
        }
    }
    return accepted;
}

bool CloudClient::handleJob(JsonObject job) {
    const char* jobId = job["jobId"] | (job["id"] | "");
    if (!validJobId(jobId)) {
        Serial.println("[CLOUD] Dropping job with invalid ID");
        return false;
    }

    ReserveResult r = g_printQueue.reserve(jobId);
    if (r == RESERVE_DUP_INFLIGHT) {
        Serial.printf("[CLOUD] Job %s redelivered while still queued/printing - ignored\n", jobId);
        return false;
    }
    if (r == RESERVE_DUP_DONE) {
        // Our /complete never reached the server (lease expired and it re-dispatched). Do NOT print again.
        Serial.printf("[CLOUD] Job %s already printed - re-reporting completion only\n", jobId);
        if (!reportComplete(jobId, 0, 2)) g_printQueue.addPendingReport(jobId, 0);
        return false;
    }

    // Everything below must release() the reservation on failure so a legitimate retry is accepted.
    auto reject = [&](const String& reason) {
        Serial.printf("[CLOUD] Rejecting job %s: %s\n", jobId, reason.c_str());
        reportFail(jobId, reason, false, 0);
        g_printQueue.release(jobId);
        {
            StateLock lock;
            if (lock.ok()) g_systemState.totalJobsFailed++;
        }
        return false;
    };

    char type[16];
    const char* rawType = job["printType"] | (job["jobType"] | "BILL");
    size_t ti = 0;
    for (; rawType[ti] && ti < sizeof(type) - 1; ti++) type[ti] = toupper((unsigned char)rawType[ti]);
    type[ti] = 0;
    PrinterIdx printer = PrintQueue::printerForType(type);

    // Resolve target. A server-supplied target is only honoured if it is a private LAN printer address.
    String host = job["targetHost"] | (job["targetPrinterHost"] | "");
    int port = job["targetPort"] | (job["targetPrinterPort"] | 0);
    if (host.length() == 0 || port <= 0) {
        StateLock lock;
        if (lock.ok()) {
            if (host.length() == 0) host = printer == PRINTER_JUICE ? g_systemState.juiceKotHost : (printer == PRINTER_KOT ? g_systemState.kotHost : g_systemState.billHost);
            if (port <= 0) port = printer == PRINTER_JUICE ? g_systemState.juiceKotPort : (printer == PRINTER_KOT ? g_systemState.kotPort : g_systemState.billPort);
        }
    }
    String why;
    if (!g_printerClient.isAllowedTarget(host, port, why)) return reject("Target refused: " + why);

    const char* b64 = job["dataBase64"] | (job["payloadBase64"] | "");
    size_t b64Len = strlen(b64);
    if (b64Len == 0) return reject("Empty print payload");
    if (b64Len > (MAX_PAYLOAD_BYTES / 3 + 1) * 4) return reject("Print payload too large");

    // Exact-size decode into PSRAM
    size_t needed = 0;
    mbedtls_base64_decode(nullptr, 0, &needed, (const unsigned char*)b64, b64Len);   // reports required size
    if (needed == 0 || needed > MAX_PAYLOAD_BYTES) return reject("Print payload size invalid");
    uint8_t* raw = (uint8_t*)psramAlloc(needed);
    if (!raw) return reject("Out of memory for print payload");
    size_t rawLen = 0;
    if (mbedtls_base64_decode(raw, needed, &rawLen, (const unsigned char*)b64, b64Len) != 0 || rawLen == 0) {
        heap_caps_free(raw);
        return reject("Base64 decode failed");
    }

    PrintJob* pj = new (std::nothrow) PrintJob();
    if (!pj) {
        heap_caps_free(raw);
        return reject("Out of memory");
    }
    memset(pj, 0, sizeof(*pj));
    strlcpy(pj->id, jobId, sizeof(pj->id));
    strlcpy(pj->type, type, sizeof(pj->type));
    strlcpy(pj->host, host.c_str(), sizeof(pj->host));
    pj->port = (uint16_t)port;
    pj->data = raw;
    pj->len = rawLen;
    pj->receivedMs = millis();

    if (!g_printQueue.submit(pj)) {
        PrintQueue::freeJob(pj);
        return reject("Bridge print queue full");
    }

    Serial.printf("[CLOUD] Queued %s job %s -> %s:%d (%u bytes)\n", type, jobId, host.c_str(), port, (unsigned)rawLen);
    reportAck(jobId);     // Extends the server-side claim lease while the job waits for its printer
    return true;
}

// ------------------------------------------------------------------------------
// Job lifecycle reports
// ------------------------------------------------------------------------------
bool CloudClient::reportAck(const char* jobId) {
    String body = "{\"bridgeId\":\"" + m_bridgeId + "\"}";
    return postWithRetry(jobPath(jobId, "ack-receipt"), body, 2);
}

bool CloudClient::reportSent(const char* jobId) {
    String body = "{\"bridgeId\":\"" + m_bridgeId + "\"}";
    return postWithRetry(jobPath(jobId, "sent"), body, 1);    // Best effort: never delay the print
}

bool CloudClient::reportComplete(const char* jobId, int bytesPrinted, int tries) {
    String body = "{\"bridgeId\":\"" + m_bridgeId + "\",\"bytesPrinted\":" + String(bytesPrinted) + "}";
    return postWithRetry(jobPath(jobId, "complete"), body, tries);
}

bool CloudClient::reportFail(const char* jobId, const String& message, bool ambiguous, int bytesPrinted) {
    JsonDocument doc;
    doc["bridgeId"] = m_bridgeId;
    doc["message"] = message;
    doc["ambiguous"] = ambiguous;
    doc["bytesPrinted"] = bytesPrinted;
    String body;
    serializeJson(doc, body);
    return postWithRetry(jobPath(jobId, "fail"), body, REPORT_TRIES_DEFAULT);
}
