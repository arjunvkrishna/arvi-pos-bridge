#include "system_state.h"
#include "config.h"
#include <esp_heap_caps.h>

SystemState g_systemState;
SemaphoreHandle_t g_stateMutex = nullptr;

void initState() {
    g_stateMutex = xSemaphoreCreateMutex();

    if (xSemaphoreTake(g_stateMutex, portMAX_DELAY) == pdTRUE) {
        g_systemState.state = STATE_BOOTING;
        g_systemState.wifiConnected = false;
        g_systemState.wifiIp = "0.0.0.0";
        g_systemState.wifiRssi = -100;
        g_systemState.wifiSsid = DEFAULT_WIFI_SSID;

        g_systemState.serverConnected = false;
        g_systemState.lastHttpCode = 0;
        g_systemState.lastServerError = "Not connected";
        g_systemState.lastServerLatencyMs = 0;
        g_systemState.successfulPolls = 0;
        g_systemState.failedPolls = 0;

        g_systemState.freeHeapBytes = esp_get_free_heap_size();
        g_systemState.freePsramBytes = ESP.getFreePsram();
        g_systemState.totalPsramBytes = ESP.getPsramSize();
        g_systemState.uptimeSeconds = 0;
        g_systemState.cpuTemperatureC = 0.0f;
        g_systemState.lastEspError = "None";

        g_systemState.billHost = DEFAULT_BILL_PRINTER_HOST;
        g_systemState.billPort = DEFAULT_BILL_PRINTER_PORT;
        g_systemState.billReachable = false;
        g_systemState.billLatencyMs = 0;
        g_systemState.billLastError = "Unchecked";

        g_systemState.kotHost = DEFAULT_KOT_PRINTER_HOST;
        g_systemState.kotPort = DEFAULT_KOT_PRINTER_PORT;
        g_systemState.kotReachable = false;
        g_systemState.kotLatencyMs = 0;
        g_systemState.kotLastError = "Unchecked";

        g_systemState.juiceKotHost = DEFAULT_JUICE_KOT_PRINTER_HOST;
        g_systemState.juiceKotPort = DEFAULT_JUICE_KOT_PRINTER_PORT;
        g_systemState.juiceKotReachable = false;
        g_systemState.juiceKotLatencyMs = 0;
        g_systemState.juiceKotLastError = "Unchecked";

        g_systemState.queueCount = 0;
        g_systemState.activeJobId = "";
        g_systemState.activeJobType = "IDLE";
        g_systemState.activeTargetHost = "";
        g_systemState.activeTargetPort = 0;
        g_systemState.activeJobBytes = 0;
        g_systemState.totalJobsPrinted = 0;
        g_systemState.totalJobsFailed = 0;

        g_systemState.hasActiveAlert = false;
        g_systemState.activeAlertTitle = "";
        g_systemState.activeAlertMessage = "";
        g_systemState.alertTimestampMs = 0;

        xSemaphoreGive(g_stateMutex);
    }
}

void updateSystemHardwareStats() {
    if (g_stateMutex == nullptr) return;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_systemState.freeHeapBytes = esp_get_free_heap_size();
        g_systemState.freePsramBytes = ESP.getFreePsram();
        g_systemState.totalPsramBytes = ESP.getPsramSize();
        g_systemState.uptimeSeconds = millis() / 1000;
        g_systemState.cpuTemperatureC = temperatureRead();
        xSemaphoreGive(g_stateMutex);
    }
}

void setAlert(const String& title, const String& message) {
    if (g_stateMutex == nullptr) return;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_systemState.hasActiveAlert = true;
        g_systemState.activeAlertTitle = title;
        g_systemState.activeAlertMessage = message;
        g_systemState.alertTimestampMs = millis();
        xSemaphoreGive(g_stateMutex);
    }
}

void clearAlert() {
    if (g_stateMutex == nullptr) return;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_systemState.hasActiveAlert = false;
        g_systemState.activeAlertTitle = "";
        g_systemState.activeAlertMessage = "";
        xSemaphoreGive(g_stateMutex);
    }
}

void clearAlertPrefix(const char* titlePrefix) {
    StateLock lock(50);
    if (lock.ok() && g_systemState.hasActiveAlert && g_systemState.activeAlertTitle.startsWith(titlePrefix)) {
        g_systemState.hasActiveAlert = false;
        g_systemState.activeAlertTitle = "";
        g_systemState.activeAlertMessage = "";
    }
}
