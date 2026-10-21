#ifndef NERAM_SYSTEM_STATE_H
#define NERAM_SYSTEM_STATE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum PrintBridgeStatusState {
    STATE_BOOTING = 0,
    STATE_CONNECTING_WIFI,
    STATE_WIFI_CONNECTED,
    STATE_IDLE,
    STATE_POLLING,
    STATE_RECEIVING_JOB,
    STATE_PRINTING,
    STATE_ACKING_JOB,
    STATE_ERROR
};

enum DisplayScreenMode {
    SCREEN_SERVER_STATUS = 0,    // 1. Webserver connection & error
    SCREEN_ESP32_SYSTEM,         // 2. ESP32 status & hardware memory
    SCREEN_DUAL_PRINTERS,        // 3. Printer connectivity & error status (Bill & KOT)
    SCREEN_PRINTING_QUEUE,       // 4. Printing status & queue count
    SCREEN_ERROR_ALERT,          // 5. Critical errors / warnings screen
    SCREEN_TOTAL_COUNT
};

struct SystemState {
    // Current Lifecycle State
    PrintBridgeStatusState state;

    // WiFi Info
    bool wifiConnected;
    String wifiIp;
    int wifiRssi;
    String wifiSsid;

    // 1. Backend / Cloud Server Status
    bool serverConnected;
    int lastHttpCode;            // 200, 401, 404, 500, etc.
    String lastServerError;      // e.g. "401 Unauthorized", "Connection Refused", "Timeout"
    uint32_t lastServerLatencyMs;
    uint32_t successfulPolls;
    uint32_t failedPolls;

    // 2. ESP32-S3 Hardware Status
    uint32_t freeHeapBytes;
    uint32_t freePsramBytes;
    uint32_t totalPsramBytes;
    uint32_t uptimeSeconds;
    float cpuTemperatureC;
    String lastEspError;         // e.g. "PSRAM Alloc Failed", "Watchdog Reset"

    // 3. Dual Printer Status (Bill & KOT)
    String billHost;
    int billPort;
    bool billReachable;
    uint32_t billLatencyMs;
    String billLastError;

    String kotHost;
    int kotPort;
    bool kotReachable;
    uint32_t kotLatencyMs;
    String kotLastError;

    String juiceKotHost;
    int juiceKotPort;
    bool juiceKotReachable;
    uint32_t juiceKotLatencyMs;
    String juiceKotLastError;

    // 4. Printing & Queue Telemetry
    uint32_t queueCount;
    String activeJobId;
    String activeJobType;        // "BILL", "KOT", "TEST"
    String activeTargetHost;
    int activeTargetPort;
    uint32_t activeJobBytes;
    uint32_t totalJobsPrinted;
    uint32_t totalJobsFailed;

    // 5. General Error Alert
    bool hasActiveAlert;
    String activeAlertTitle;
    String activeAlertMessage;
    uint32_t alertTimestampMs;
};

// Global shared state and mutex declaration
extern SystemState g_systemState;
extern SemaphoreHandle_t g_stateMutex;

void initState();
void updateSystemHardwareStats();
void setAlert(const String& title, const String& message);
void clearAlert();
void clearAlertPrefix(const char* titlePrefix);   // clears only if the active alert title starts with prefix

// RAII lock for g_systemState. Check ok() before touching state.
class StateLock {
public:
    explicit StateLock(uint32_t timeoutMs = 100)
        : m_ok(g_stateMutex != nullptr && xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {}
    ~StateLock() { if (m_ok) xSemaphoreGive(g_stateMutex); }
    bool ok() const { return m_ok; }
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;
private:
    bool m_ok;
};

#endif // NERAM_SYSTEM_STATE_H
