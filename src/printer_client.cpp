#include "printer_client.h"
#include "config.h"
#include "system_state.h"
#include <WiFi.h>

PrinterClient g_printerClient;

PrinterClient::PrinterClient() {
    m_tableMutex = xSemaphoreCreateMutex();
}

static bool isPrivateIPv4(const IPAddress& ip) {
    return ip[0] == 10 ||
           (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) ||
           (ip[0] == 192 && ip[1] == 168);
}

bool PrinterClient::isAllowedTarget(const String& host, int port, String& why) {
    IPAddress ip;
    if (host.length() == 0 || !ip.fromString(host)) {
        why = "Target must be an IPv4 address";
        return false;
    }
    if (!isPrivateIPv4(ip)) {
        why = "Target is not a private LAN address";
        return false;
    }
    if (port < PRINTER_PORT_MIN || port > PRINTER_PORT_MAX) {
        why = "Target port outside allowed printer range";
        return false;
    }
    if (ip[3] == 0 || ip[3] == 255) {
        why = "Target is a network/broadcast address";
        return false;
    }
    if (WiFi.status() == WL_CONNECTED && (ip == WiFi.localIP() || ip == WiFi.gatewayIP())) {
        why = "Target is this bridge or the gateway";
        return false;
    }
    return true;
}

SemaphoreHandle_t PrinterClient::lockFor(uint32_t ip, uint16_t port) {
    SemaphoreHandle_t result = nullptr;
    if (xSemaphoreTake(m_tableMutex, portMAX_DELAY) == pdTRUE) {
        for (auto& s : m_slots) {
            if (s.mutex && s.ip == ip && s.port == port) { result = s.mutex; break; }
        }
        if (!result) {
            for (auto& s : m_slots) {
                if (!s.mutex) {
                    s.ip = ip; s.port = port; s.mutex = xSemaphoreCreateMutex();
                    result = s.mutex;
                    break;
                }
            }
        }
        xSemaphoreGive(m_tableMutex);
    }
    return result;
}

PrinterCheckResult PrinterClient::checkPrinter(const String& host, int port, uint32_t timeoutMs) {
    PrinterCheckResult result{false, 0, ""};
    if (host.length() == 0) {
        result.errorMessage = "Not configured";
        return result;
    }
    String why;
    if (!isAllowedTarget(host, port, why)) {
        result.errorMessage = why;
        return result;
    }

    WiFiClient client;
    uint32_t startMs = millis();
    bool connected = client.connect(host.c_str(), port, timeoutMs);
    result.latencyMs = millis() - startMs;
    if (connected) {
        result.reachable = true;
        client.stop();
    } else {
        result.errorMessage = "Connection refused or timed out";
    }
    return result;
}

PrintResult PrinterClient::sendRawPrintJob(const String& host, int port, const uint8_t* data, size_t length,
                                           const std::function<void()>& onConnected) {
    PrintResult res;
    String why;
    if (!isAllowedTarget(host, port, why)) {
        res.outcome = PRINT_REJECTED;
        res.error = why;
        return res;
    }
    if (data == nullptr || length == 0) {
        res.outcome = PRINT_REJECTED;
        res.error = "No print payload data provided";
        return res;
    }

    IPAddress ip;
    ip.fromString(host);
    SemaphoreHandle_t lock = lockFor((uint32_t)ip, (uint16_t)port);
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);

    uint32_t startMs = millis();
    WiFiClient client;
    // NOTE: WiFiClient::setTimeout() takes SECONDS in the Arduino-ESP32 2.x core.
    client.setTimeout((PRINTER_SOCKET_TIMEOUT_MS + 999) / 1000);

    bool connected = false;
    for (int attempt = 0; attempt < PRINTER_CONNECT_ATTEMPTS && !connected; attempt++) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(attempt == 1 ? 150 : 400));
        connected = client.connect(host.c_str(), port, PRINTER_SOCKET_TIMEOUT_MS);
    }

    if (!connected) {
        res.outcome = PRINT_CONNECT_FAILED;
        res.error = "TCP connect to " + host + ":" + String(port) + " failed";
        res.durationMs = millis() - startMs;
        if (lock) xSemaphoreGive(lock);
        return res;
    }

    client.setNoDelay(true);   // No Nagle: small ESC/POS tails go out immediately
    if (onConnected) onConnected();

    size_t sent = 0;
    bool failed = false;
    while (sent < length) {
        size_t toSend = min((size_t)PRINTER_CHUNK_BYTES, length - sent);
        size_t written = client.write(data + sent, toSend);
        if (written == 0) {
            // One short retry in case of a transient lwip buffer-full condition
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!client.connected() || (written = client.write(data + sent, toSend)) == 0) {
                failed = true;
                break;
            }
        }
        sent += written;
    }

    if (!failed) {
        client.flush();
        vTaskDelay(pdMS_TO_TICKS(PRINTER_CLOSE_LINGER_MS));
    }
    client.stop();
    if (lock) xSemaphoreGive(lock);

    res.bytesSent = sent;
    res.durationMs = millis() - startMs;
    if (failed) {
        res.outcome = PRINT_WRITE_FAILED;
        res.error = "Socket write failed after " + String((unsigned)sent) + "/" + String((unsigned)length) + " bytes";
    } else {
        res.outcome = PRINT_OK;
    }
    return res;
}

void PrinterClient::checkPrinters(bool skipBill, bool skipKot, bool skipJuice) {
    String host[3];
    int port[3] = {DEFAULT_BILL_PRINTER_PORT, DEFAULT_KOT_PRINTER_PORT, DEFAULT_JUICE_KOT_PRINTER_PORT};
    {
        StateLock lock;
        if (!lock.ok()) return;
        host[0] = g_systemState.billHost;      port[0] = g_systemState.billPort;
        host[1] = g_systemState.kotHost;       port[1] = g_systemState.kotPort;
        host[2] = g_systemState.juiceKotHost;  port[2] = g_systemState.juiceKotPort;
    }

    bool skip[3] = {skipBill, skipKot, skipJuice};
    PrinterCheckResult res[3];
    for (int i = 0; i < 3; i++) {
        res[i] = PrinterCheckResult{false, 0, ""};
        if (!skip[i]) res[i] = checkPrinter(host[i], port[i]);
    }

    StateLock lock;
    if (!lock.ok()) return;
    if (!skip[0]) { g_systemState.billReachable = res[0].reachable; g_systemState.billLatencyMs = res[0].latencyMs;
                    g_systemState.billLastError = res[0].reachable ? "OK" : res[0].errorMessage; }
    if (!skip[1]) { g_systemState.kotReachable = res[1].reachable; g_systemState.kotLatencyMs = res[1].latencyMs;
                    g_systemState.kotLastError = res[1].reachable ? "OK" : res[1].errorMessage; }
    if (!skip[2]) { g_systemState.juiceKotReachable = res[2].reachable; g_systemState.juiceKotLatencyMs = res[2].latencyMs;
                    g_systemState.juiceKotLastError = res[2].reachable ? "OK" : res[2].errorMessage; }

    // Clear a stale PRINTER FAIL alert once every printer that has an address is reachable again
    bool allUp = (host[0].isEmpty() || g_systemState.billReachable) &&
                 (host[1].isEmpty() || g_systemState.kotReachable) &&
                 (host[2].isEmpty() || g_systemState.juiceKotReachable);
    if (allUp && g_systemState.hasActiveAlert && g_systemState.activeAlertTitle.startsWith("PRINTER FAIL")) {
        g_systemState.hasActiveAlert = false;
        g_systemState.activeAlertTitle = "";
        g_systemState.activeAlertMessage = "";
    }
}
