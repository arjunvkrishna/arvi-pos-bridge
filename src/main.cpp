#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "system_state.h"
#include "display_manager.h"
#include "printer_client.h"
#include "cloud_client.h"
#include "rgb_led_manager.h"
#include "print_queue.h"
#include <esp_task_wdt.h>

// FreeRTOS Task Handles
TaskHandle_t g_networkTaskHandle = nullptr;
TaskHandle_t g_displayTaskHandle = nullptr;
TaskHandle_t g_maintTaskHandle = nullptr;

// ------------------------------------------------------------------------------
// WiFi Connection Helper
// ------------------------------------------------------------------------------
void connectWiFi() {
    g_rgbLed.setState(LED_STATE_CONNECTING_WIFI);
    {
        StateLock lock(50);
        if (lock.ok()) {
            g_systemState.state = STATE_CONNECTING_WIFI;
            g_systemState.wifiConnected = false;
        }
    }

    Serial.printf("[WIFI] Connecting to SSID: %s ...\n", DEFAULT_WIFI_SSID);
    WiFi.disconnect(true, true);
    vTaskDelay(pdMS_TO_TICKS(200));
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);              // Modem sleep adds 100-300ms to the first packet of every job
    WiFi.setAutoReconnect(true);

    static bool eventHooked = false;   // Log why Wi-Fi dropped (auth fail, AP not found, ...)
    if (!eventHooked) {
        eventHooked = true;
        WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
            if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
                Serial.printf("[WIFI-EVENT] Disconnected! Reason code: %d\n", info.wifi_sta_disconnected.reason);
            }
        });
    }
    WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs < WIFI_CONNECT_TIMEOUT_MS)) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] Connected! IP: %s | RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        g_rgbLed.setState(LED_STATE_CONNECTING_POS);
        {
            StateLock lock(50);
            if (lock.ok()) {
                g_systemState.wifiConnected = true;
                g_systemState.wifiIp = WiFi.localIP().toString();
                g_systemState.wifiRssi = WiFi.RSSI();
                g_systemState.state = STATE_IDLE;
            }
        }
        clearAlertPrefix("WIFI CONNECT FAIL");
    } else {
        Serial.printf("[WIFI] Connection FAILED! status=%d\n", WiFi.status());
        int n = WiFi.scanNetworks();
        Serial.printf("[WIFI] Found %d visible networks:\n", n);
        for (int i = 0; i < n; ++i) {
            Serial.printf("  [%d] SSID: '%s' | RSSI: %d dBm | Ch: %d | Enc: %d\n",
                          i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i));
        }
        g_rgbLed.setState(LED_STATE_ALERT);
        {
            StateLock lock(50);
            if (lock.ok()) {
                g_systemState.wifiConnected = false;
                g_systemState.state = STATE_ERROR;
            }
        }
        setAlert("WIFI CONNECT FAIL", DEFAULT_WIFI_SSID);
    }
}

// ------------------------------------------------------------------------------
// FreeRTOS Core 1 Task: OLED Display Loop (~15 FPS) + Onboard RGB LED Controller
// ------------------------------------------------------------------------------
void displayTask(void* parameter) {
    Serial.println("[TASK] Display Task started on Core 1");
#if OLED_ENABLED
    g_display.begin();
#else
    Serial.println("[DISPLAY] Disabled (OLED_ENABLED=0); status LED only");
#endif

    const TickType_t xDelay = pdMS_TO_TICKS(1000 / OLED_REFRESH_FPS);

    while (true) {
#if OLED_ENABLED
        g_display.update();
#endif
        g_rgbLed.update();
        vTaskDelay(xDelay);
    }
}

// ------------------------------------------------------------------------------
// Core 0 Task: cloud long-poll only. Everything slow (heartbeat, config sync,
// printer probes) lives in the maintenance task so it can never delay a job.
// ------------------------------------------------------------------------------
void networkTask(void* parameter) {
    Serial.println("[TASK] Network (poll) Task started on Core 0");
    esp_task_wdt_add(nullptr);

    uint32_t backoffMs = 0;

    while (true) {
        esp_task_wdt_reset();

        if (WiFi.status() != WL_CONNECTED) {
            connectWiFi();
            if (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!g_cloudClient.ensureTime(2000)) continue;   // TLS needs a valid clock

        // Backpressure: don't claim more jobs than the local queues can hold
        if (!g_printQueue.hasRoom()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint32_t startMs = millis();
        int result = g_cloudClient.pollOnce();
        uint32_t elapsed = millis() - startMs;

        if (result < 0) {
            backoffMs = backoffMs == 0 ? POLL_BACKOFF_MIN_MS : min<uint32_t>(backoffMs * 2, POLL_BACKOFF_MAX_MS);
            Serial.printf("[CLOUD] Poll failed, retrying in %ums\n", (unsigned)backoffMs);
            for (uint32_t waited = 0; waited < backoffMs; waited += 500) {
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        } else {
            backoffMs = 0;
            if (result == 0 && elapsed < POLL_MIN_CYCLE_MS) {
                vTaskDelay(pdMS_TO_TICKS(POLL_MIN_CYCLE_MS - elapsed));   // Server answered 204 without holding the poll
            } else {
                taskYIELD();
            }
        }
    }
}

// ------------------------------------------------------------------------------
// Core 0 Task: heartbeat, printer config sync, printer health, pending reports
// ------------------------------------------------------------------------------
void maintenanceTask(void* parameter) {
    Serial.println("[TASK] Maintenance Task started on Core 0");
    esp_task_wdt_add(nullptr);

    uint32_t lastHeartbeat = 0, lastCheck = 0, lastConfig = 0, lastFlush = 0, lastWifi = 0;
    bool first = true;

    while (true) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));

        if (WiFi.status() != WL_CONNECTED) continue;
        uint32_t now = millis();

        if (now - lastWifi > 5000) {
            lastWifi = now;
            StateLock lock(20);
            if (lock.ok()) {
                g_systemState.wifiConnected = true;
                g_systemState.wifiIp = WiFi.localIP().toString();
                g_systemState.wifiRssi = WiFi.RSSI();
            }
        }

        if (!g_cloudClient.ensureTime(0)) continue;

        if (first || now - lastConfig > CONFIG_SYNC_INTERVAL_MS) {
            lastConfig = now;
            g_cloudClient.fetchPrinterConfig();
        }
        if (first || now - lastCheck > PRINTER_CHECK_INTERVAL_MS) {
            lastCheck = now;
            // A printer that is mid-job is skipped: probing it would open a second connection to it
            g_printerClient.checkPrinters(g_printQueue.isBusy(PRINTER_BILL), g_printQueue.isBusy(PRINTER_KOT), g_printQueue.isBusy(PRINTER_JUICE));
        }
        if (first || now - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
            lastHeartbeat = now;
            g_cloudClient.sendHeartbeat();
        }
        if (now - lastFlush > PENDING_REPORT_RETRY_MS) {
            lastFlush = now;
            g_printQueue.flushPendingReports();
        }
        first = false;
    }
}

// ------------------------------------------------------------------------------
// Arduino Setup & Loop
// ------------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=======================================================");
    Serial.println("  NERAM POS DEDICATED PRINT BRIDGE - ESP32-S3-N16R8    ");
    Serial.printf("  Firmware: %s | Model: %s\n", FIRMWARE_VERSION, HARDWARE_MODEL);
    Serial.println("=======================================================");

    // Verify 8MB Octal PSRAM
    if (psramInit()) {
        Serial.printf("[HARDWARE] PSRAM Initialized successfully! Total: %u MB, Free: %u MB\n",
            ESP.getPsramSize() / (1024 * 1024),
            ESP.getFreePsram() / (1024 * 1024));
    } else {
        Serial.println("[HARDWARE] WARNING: PSRAM Initialization failed or disabled!");
    }
    Serial.printf("[HARDWARE] Free Internal Heap: %u KB\n", esp_get_free_heap_size() / 1024);

    // Initialize Shared State
    initState();

    // Initialize Onboard RGB LED Manager
    g_rgbLed.begin();

    // Task watchdog: any worker stuck for TASK_WDT_TIMEOUT_S reboots the bridge
    esp_task_wdt_init(TASK_WDT_TIMEOUT_S, true);

    g_cloudClient.begin();
    g_printQueue.begin();

    // Spawn Core 1 Display Task
    xTaskCreatePinnedToCore(
        displayTask,
        "DisplayTask",
        DISPLAY_TASK_STACK,
        nullptr,
        1,
        &g_displayTaskHandle,
        DISPLAY_TASK_CORE
    );

    // Spawn Core 0 Network & Printing Task
    xTaskCreatePinnedToCore(
        networkTask,
        "NetworkTask",
        NETWORK_TASK_STACK,
        nullptr,
        2,
        &g_networkTaskHandle,
        NETWORK_TASK_CORE
    );

    xTaskCreatePinnedToCore(
        maintenanceTask,
        "MaintTask",
        MAINT_TASK_STACK,
        nullptr,
        1,
        &g_maintTaskHandle,
        NETWORK_TASK_CORE
    );
}

void loop() {
    // FreeRTOS tasks handle all work. Delay loop task.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
