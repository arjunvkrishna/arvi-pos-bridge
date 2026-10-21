#ifndef NERAM_CLOUD_CLIENT_H
#define NERAM_CLOUD_CLIENT_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

class CappedBuffer;

class CloudClient {
public:
    CloudClient();

    // Creates the (persistent) HTTP channels and loads the root CA. Call once before use.
    void begin();
    const String& bridgeId() const { return m_bridgeId; }

    // TLS needs a valid clock. Returns true once SNTP has synced (starts it on first call).
    bool ensureTime(uint32_t waitMs);

    // Housekeeping calls (shared persistent connection, serialized by a mutex)
    bool fetchPrinterConfig();
    bool sendHeartbeat();

    // Long-polls for one job. Returns: -1 error, 0 nothing to do, >0 jobs accepted into the local queue.
    int pollOnce();

    // Job lifecycle reports (retried on transient errors)
    bool reportAck(const char* jobId);
    bool reportSent(const char* jobId);
    bool reportComplete(const char* jobId, int bytesPrinted, int tries = REPORT_TRIES_DEFAULT);
    bool reportFail(const char* jobId, const String& message, bool ambiguous, int bytesPrinted);

private:
    static const int REPORT_TRIES_DEFAULT = 4;

    struct Channel {
        WiFiClient plain;
        WiFiClientSecure tls;
        HTTPClient http;
        bool secure = true;
        WiFiClient& client() { return secure ? static_cast<WiFiClient&>(tls) : plain; }
    };

    int request(Channel& ch, bool post, const String& path, const String* body, uint32_t timeoutMs, CappedBuffer* out);
    int apiRequest(bool post, const String& path, const String* body, CappedBuffer* out = nullptr);
    bool postWithRetry(const String& path, const String& body, int tries);
    String jobPath(const char* jobId, const char* action) const;

    void recordPollOk(uint32_t latencyMs);
    void recordPollFailure(int code);
    int handlePollBody(CappedBuffer& body);
    bool handleJob(JsonObject job);

    Channel m_poll;
    Channel m_api;
    SemaphoreHandle_t m_apiMutex;
    String m_bridgeId;
    bool m_ntpStarted = false;
};

extern CloudClient g_cloudClient;

#endif // NERAM_CLOUD_CLIENT_H
