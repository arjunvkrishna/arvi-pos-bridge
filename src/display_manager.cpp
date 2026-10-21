#include "display_manager.h"
#include "config.h"

// Instantiate U8G2 display driver
#if defined(OLED_USE_SSD1306)
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2_instance(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN);
#else
// Default 1.3" OLED displays use SH1106 controller
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2_instance(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN);
#endif

DisplayManager g_display;

DisplayManager::DisplayManager()
    : m_u8g2(&u8g2_instance),
      m_currentScreen(SCREEN_SERVER_STATUS),
      m_lastCycleTime(0),
      m_displayFound(false),
      m_lastProbeTime(0),
      m_activeSda(OLED_SDA_PIN),
      m_activeScl(OLED_SCL_PIN),
      m_activeAddr(OLED_I2C_ADDR) {}

bool DisplayManager::probeI2C(uint8_t sdaPin, uint8_t sclPin, uint8_t addr7bit) {
    auto setSda = [&](int v) {
        if (v) {
            pinMode(sdaPin, INPUT_PULLUP);
        } else {
            pinMode(sdaPin, OUTPUT);
            digitalWrite(sdaPin, LOW);
        }
    };
    auto setScl = [&](int v) {
        if (v) {
            pinMode(sclPin, INPUT_PULLUP);
        } else {
            pinMode(sclPin, OUTPUT);
            digitalWrite(sclPin, LOW);
        }
    };
    auto getSda = [&]() -> int {
        pinMode(sdaPin, INPUT_PULLUP);
        return digitalRead(sdaPin);
    };

    setSda(1);
    setScl(1);
    delayMicroseconds(25);
    if (getSda() == 0 || digitalRead(sclPin) == 0) {
        return false;
    }

    // START
    setSda(0);
    delayMicroseconds(8);
    setScl(0);
    delayMicroseconds(8);

    // Send 7-bit addr + 0 (Write)
    uint8_t b = (addr7bit << 1);
    for (int i = 7; i >= 0; i--) {
        setSda((b >> i) & 1);
        delayMicroseconds(8);
        setScl(1);
        delayMicroseconds(8);
        setScl(0);
        delayMicroseconds(8);
    }

    // Read ACK on 9th clock
    setSda(1);
    delayMicroseconds(8);
    setScl(1);
    delayMicroseconds(8);
    int ack = getSda();
    setScl(0);
    delayMicroseconds(8);

    // STOP
    setSda(0);
    delayMicroseconds(8);
    setScl(1);
    delayMicroseconds(8);
    setSda(1);
    delayMicroseconds(25);

    return (ack == 0);
}

bool DisplayManager::tryDetectDisplay() {
    // Use only the documented wiring. Sweeping arbitrary GPIO pairs and
    // manually clocking the bus can disturb other board peripherals and makes
    // OLED startup harder to diagnose.
    constexpr uint8_t addresses[] = {0x3C, 0x3D};
    struct I2cPins { uint8_t sda; uint8_t scl; };
    const I2cPins pinPairs[] = {
        {OLED_SDA_PIN, OLED_SCL_PIN},
        {OLED_SCL_PIN, OLED_SDA_PIN} // Also diagnose accidentally swapped SDA/SCL wires.
    };

    for (const I2cPins& pins : pinPairs) {
        Wire.end();
        if (!Wire.begin(pins.sda, pins.scl, OLED_I2C_FREQ)) {
            Serial.printf("[DISPLAY] I2C begin failed (SDA=GPIO%d, SCL=GPIO%d)\n",
                          pins.sda, pins.scl);
            continue;
        }
        Wire.setTimeOut(50);
        Serial.printf("[DISPLAY] Probing OLED on SDA=GPIO%d, SCL=GPIO%d\n",
                      pins.sda, pins.scl);

        for (uint8_t address : addresses) {
            Wire.beginTransmission(address);
            const uint8_t error = Wire.endTransmission();
            Serial.printf("[DISPLAY] I2C address 0x%02X: %s (code %u)\n",
                          address, error == 0 ? "ACK" : "no response", error);
            if (error != 0) continue;

            m_activeSda = pins.sda;
            m_activeScl = pins.scl;
            m_activeAddr = address;
            m_displayFound = true;

        // Let the selected U8g2 controller driver perform the complete panel
        // initialization. Do not send mixed SSD1306/SH1106 commands manually.
            u8x8_SetPin_HW_I2C(m_u8g2->getU8x8(), U8X8_PIN_NONE,
                               m_activeScl, m_activeSda);
            m_u8g2->setI2CAddress(m_activeAddr * 2);
            m_u8g2->begin();
            m_u8g2->setPowerSave(0);
            m_u8g2->setContrast(255);
            m_u8g2->clearBuffer();
            m_u8g2->setFont(u8g2_font_6x10_tf);
            m_u8g2->drawStr(4, 18, "NERAM POS BRIDGE");
            m_u8g2->drawStr(4, 32, "ESP32-S3-N16R8");
            m_u8g2->drawStr(4, 46, "16MB Flash | 8MB PSRAM");
            m_u8g2->drawStr(4, 60, "Initializing...");
            m_u8g2->sendBuffer();

            Serial.printf("[DISPLAY] OLED initialized at 0x%02X, SDA=GPIO%d, SCL=GPIO%d\n",
                          m_activeAddr, m_activeSda, m_activeScl);
            if (g_stateMutex && xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                char buf[64];
                snprintf(buf, sizeof(buf), "OLED ONLINE: 0x%02X, GPIO%d/%d",
                         address, m_activeSda, m_activeScl);
                g_systemState.lastEspError = buf;
                xSemaphoreGive(g_stateMutex);
            }
            return true;
        }
    }

    m_displayFound = false;
    Serial.println("[DISPLAY] No OLED ACK at 0x3C or 0x3D; will retry in 2 seconds");
    if (g_stateMutex && xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        char buf[48];
        snprintf(buf, sizeof(buf), "NO OLED ACK GPIO%d/%d", OLED_SDA_PIN, OLED_SCL_PIN);
        g_systemState.lastEspError = buf;
        xSemaphoreGive(g_stateMutex);
    }
    return false;
}

bool DisplayManager::begin() {
    m_lastProbeTime = millis();
    tryDetectDisplay();
    return true;
}

void DisplayManager::setScreen(DisplayScreenMode mode) {
    m_currentScreen = mode;
    m_lastCycleTime = millis();
}

void DisplayManager::nextScreen() {
    int next = (int)m_currentScreen + 1;
    if (next >= SCREEN_ERROR_ALERT) {
        next = 0;
    }
    m_currentScreen = (DisplayScreenMode)next;
    m_lastCycleTime = millis();
}

void DisplayManager::update() {
    // If display is not detected yet, periodically re-probe in background every 2s
    if (!m_displayFound) {
        if (millis() - m_lastProbeTime > 2000) {
            m_lastProbeTime = millis();
            tryDetectDisplay();
        }
        return;
    }

    // Make a local snapshot of system state safely
    SystemState state;
    if (g_stateMutex && xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(40)) == pdTRUE) {
        state = g_systemState;
        xSemaphoreGive(g_stateMutex);
    } else {
        return;
    }

    // Auto-cycle pages when not showing active critical alert
    if (state.hasActiveAlert) {
        m_currentScreen = SCREEN_ERROR_ALERT;
    } else {
        if (millis() - m_lastCycleTime > SCREEN_CYCLE_TIME_MS) {
            nextScreen();
        }
    }

    m_u8g2->clearBuffer();

    // Top status bar
    drawStatusBar(state);

    // Active Screen
    switch (m_currentScreen) {
        case SCREEN_SERVER_STATUS:
            drawScreenServer(state);
            break;
        case SCREEN_ESP32_SYSTEM:
            drawScreenESP32(state);
            break;
        case SCREEN_DUAL_PRINTERS:
            drawScreenPrinters(state);
            break;
        case SCREEN_PRINTING_QUEUE:
            drawScreenPrinting(state);
            break;
        case SCREEN_ERROR_ALERT:
            drawScreenAlert(state);
            break;
        default:
            drawScreenServer(state);
            break;
    }

    m_u8g2->sendBuffer();
}

void DisplayManager::drawStatusBar(const SystemState& state) {
    m_u8g2->setFont(u8g2_font_5x7_tf);

    // WiFi Indicator
    if (state.wifiConnected) {
        m_u8g2->drawStr(0, 7, "WiFi:OK");
    } else {
        m_u8g2->drawStr(0, 7, "WiFi:NO");
    }

    // State Badge
    const char* stateStr = "IDLE";
    if (state.state == STATE_PRINTING) {
        stateStr = "PRINT";
    } else if (state.state == STATE_POLLING) {
        stateStr = "POLL";
    } else if (state.state == STATE_CONNECTING_WIFI) {
        stateStr = "BOOT";
    } else if (state.state == STATE_ERROR) {
        stateStr = "ERR";
    }
    m_u8g2->drawStr(50, 7, stateStr);

    // Page indicator [1/4]
    char pageBuf[12];
    if (m_currentScreen == SCREEN_ERROR_ALERT) {
        snprintf(pageBuf, sizeof(pageBuf), "[ALERT]");
    } else {
        snprintf(pageBuf, sizeof(pageBuf), "[%d/4]", (int)m_currentScreen + 1);
    }
    m_u8g2->drawStr(98, 7, pageBuf);

    // Horizontal separator
    m_u8g2->drawHLine(0, 9, 128);
}

// Screen 1: Webserver Connection and Error
void DisplayManager::drawScreenServer(const SystemState& state) {
    m_u8g2->setFont(u8g2_font_6x10_tf);
    m_u8g2->drawStr(0, 20, "1. CLOUD SERVER");

    m_u8g2->setFont(u8g2_font_5x7_tf);
    char buf[40];

    // Status / HTTP Code
    if (state.serverConnected && state.lastHttpCode == 200) {
        snprintf(buf, sizeof(buf), "HTTP: 200 OK (%ums)", state.lastServerLatencyMs);
    } else if (state.lastHttpCode > 0) {
        snprintf(buf, sizeof(buf), "HTTP: %d (ERR!)", state.lastHttpCode);
    } else {
        snprintf(buf, sizeof(buf), "Status: DISCONNECTED");
    }
    m_u8g2->drawStr(0, 31, buf);

    // Server host snippet
    snprintf(buf, sizeof(buf), "Host: %s", BACKEND_BASE_URL);
    // Truncate to fit 128px
    if (strlen(buf) > 23) buf[23] = '\0';
    m_u8g2->drawStr(0, 42, buf);

    // Poll counts & errors
    snprintf(buf, sizeof(buf), "Polls: %u | Fail: %u", state.successfulPolls, state.failedPolls);
    m_u8g2->drawStr(0, 53, buf);

    // Error detail if any
    if (state.failedPolls > 0 || state.lastHttpCode != 200) {
        snprintf(buf, sizeof(buf), "Err: %s", state.lastServerError.c_str());
        if (strlen(buf) > 23) buf[23] = '\0';
        m_u8g2->drawStr(0, 63, buf);
    } else {
        m_u8g2->drawStr(0, 63, "Bridge Link: HEALTHY");
    }
}

// Screen 2: ESP32 Hardware Status & Error
void DisplayManager::drawScreenESP32(const SystemState& state) {
    m_u8g2->setFont(u8g2_font_6x10_tf);
    m_u8g2->drawStr(0, 20, "2. ESP32-S3-N16R8");

    m_u8g2->setFont(u8g2_font_5x7_tf);
    char buf[40];

    // WiFi IP Address
    snprintf(buf, sizeof(buf), "IP: %s", state.wifiIp.c_str());
    m_u8g2->drawStr(0, 31, buf);

    // PSRAM: 8MB status
    float freePsramMb = (float)state.freePsramBytes / (1024.0f * 1024.0f);
    float totalPsramMb = (float)state.totalPsramBytes / (1024.0f * 1024.0f);
    snprintf(buf, sizeof(buf), "PSRAM: %.1fMB / %.1fMB", freePsramMb, totalPsramMb);
    m_u8g2->drawStr(0, 42, buf);

    // Internal Heap & RSSI
    uint32_t freeHeapKb = state.freeHeapBytes / 1024;
    snprintf(buf, sizeof(buf), "Heap: %uKB | Sig: %ddBm", freeHeapKb, state.wifiRssi);
    m_u8g2->drawStr(0, 53, buf);

    // Uptime & System Health
    uint32_t hrs = state.uptimeSeconds / 3600;
    uint32_t mins = (state.uptimeSeconds % 3600) / 60;
    uint32_t secs = state.uptimeSeconds % 60;
    snprintf(buf, sizeof(buf), "Up: %02uh%02um%02us | %s", hrs, mins, secs, state.lastEspError.c_str());
    if (strlen(buf) > 23) buf[23] = '\0';
    m_u8g2->drawStr(0, 63, buf);
}

// Screen 3: Triple Printer Connectivity & Status (Bill, Kitchen KOT & Juice KOT)
void DisplayManager::drawScreenPrinters(const SystemState& state) {
    m_u8g2->setFont(u8g2_font_6x10_tf);
    m_u8g2->drawStr(0, 20, "3. THERMAL PRINTERS");

    m_u8g2->setFont(u8g2_font_5x7_tf);
    char buf[40];

    // BILL PRINTER
    snprintf(buf, sizeof(buf), "B: %s %s:%d", state.billReachable ? "OK" : "ERR", state.billHost.c_str(), state.billPort);
    if (strlen(buf) > 23) buf[23] = '\0';
    m_u8g2->drawStr(0, 31, buf);

    // KITCHEN KOT PRINTER
    snprintf(buf, sizeof(buf), "K: %s %s:%d", state.kotReachable ? "OK" : "ERR", state.kotHost.c_str(), state.kotPort);
    if (strlen(buf) > 23) buf[23] = '\0';
    m_u8g2->drawStr(0, 42, buf);

    // JUICE KOT PRINTER
    snprintf(buf, sizeof(buf), "J: %s %s:%d", state.juiceKotReachable ? "OK" : "ERR", state.juiceKotHost.c_str(), state.juiceKotPort);
    if (strlen(buf) > 23) buf[23] = '\0';
    m_u8g2->drawStr(0, 53, buf);

    // Latency summary
    snprintf(buf, sizeof(buf), "Lat: B:%ums K:%ums J:%ums", state.billLatencyMs, state.kotLatencyMs, state.juiceKotLatencyMs);
    if (strlen(buf) > 23) buf[23] = '\0';
    m_u8g2->drawStr(0, 63, buf);
}

// Screen 4: Printing Status and Queue Count
void DisplayManager::drawScreenPrinting(const SystemState& state) {
    m_u8g2->setFont(u8g2_font_6x10_tf);
    m_u8g2->drawStr(0, 20, "4. PRINT SPOOLER");

    m_u8g2->setFont(u8g2_font_5x7_tf);
    char buf[40];

    // Queue Count & State
    snprintf(buf, sizeof(buf), "Queue: %u | Act: %s", state.queueCount, state.activeJobType.c_str());
    m_u8g2->drawStr(0, 31, buf);

    // Active Job details
    if (state.state == STATE_PRINTING && state.activeJobId.length() > 0) {
        snprintf(buf, sizeof(buf), "Job: %s", state.activeJobId.c_str());
        if (strlen(buf) > 23) buf[23] = '\0';
        m_u8g2->drawStr(0, 42, buf);

        snprintf(buf, sizeof(buf), "Sending: %u Bytes...", state.activeJobBytes);
        m_u8g2->drawStr(0, 53, buf);
    } else {
        m_u8g2->drawStr(0, 42, "Current: IDLE / READY");
        snprintf(buf, sizeof(buf), "Last Sent: %u Bytes", state.activeJobBytes);
        m_u8g2->drawStr(0, 53, buf);
    }

    // Lifetime summary
    snprintf(buf, sizeof(buf), "Done: %u | Failed: %u", state.totalJobsPrinted, state.totalJobsFailed);
    m_u8g2->drawStr(0, 63, buf);
}

// Screen 5: Other Errors / Critical Alert Screen
void DisplayManager::drawScreenAlert(const SystemState& state) {
    // Inverted banner header
    m_u8g2->drawBox(0, 11, 128, 12);
    m_u8g2->setDrawColor(0);
    m_u8g2->setFont(u8g2_font_6x10_tf);
    m_u8g2->drawStr(4, 21, "! ERROR / ALERT !");
    m_u8g2->setDrawColor(1);

    m_u8g2->setFont(u8g2_font_5x7_tf);
    char buf[40];

    snprintf(buf, sizeof(buf), "Cause: %s", state.activeAlertTitle.c_str());
    if (strlen(buf) > 23) buf[23] = '\0';
    m_u8g2->drawStr(0, 36, buf);

    snprintf(buf, sizeof(buf), "Info : %s", state.activeAlertMessage.c_str());
    if (strlen(buf) > 23) buf[23] = '\0';
    m_u8g2->drawStr(0, 48, buf);

    m_u8g2->drawStr(0, 60, "Check WiFi / IP / Power");
}
