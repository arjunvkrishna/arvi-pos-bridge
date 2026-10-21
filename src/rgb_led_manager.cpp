#include "rgb_led_manager.h"
#include "system_state.h"

RgbLedManager g_rgbLed;

RgbLedManager::RgbLedManager()
    : m_currentState(LED_STATE_READY_ONLINE),
      m_lastBlinkMs(0),
      m_blinkPhase(false),
      m_transientExpiryMs(0),
      m_isTransient(false),
      m_currentR(255),
      m_currentG(255),
      m_currentB(255) {}

void RgbLedManager::begin() {
    m_currentR = 0;
    m_currentG = 0;
    m_currentB = 0;

#ifdef RGB_BUILTIN
    neopixelWrite(RGB_BUILTIN, 0, 0, 0);
#endif
    pinMode(48, OUTPUT);
    digitalWrite(48, LOW);
}

void RgbLedManager::writeColor(uint8_t r, uint8_t g, uint8_t b) {
    if (r == m_currentR && g == m_currentG && b == m_currentB) {
        return;
    }
    m_currentR = r;
    m_currentG = g;
    m_currentB = b;

#ifdef RGB_BUILTIN
    neopixelWrite(RGB_BUILTIN, r, g, b);
#else
    neopixelWrite(48, r, g, b);
#endif
}

void RgbLedManager::triggerPrintSuccess() {
    m_currentState = LED_STATE_PRINT_COMPLETED;
    m_isTransient = true;
    m_transientExpiryMs = millis() + 1500;
    m_lastBlinkMs = millis();
    m_blinkPhase = true;
}

void RgbLedManager::triggerPrintFailed() {
    m_currentState = LED_STATE_PRINT_FAILED;
    m_isTransient = true;
    m_transientExpiryMs = millis() + 2000;
    m_lastBlinkMs = millis();
    m_blinkPhase = true;
}

void RgbLedManager::triggerPrinting(bool active) {
    if (active) {
        m_currentState = LED_STATE_PRINTING;
        m_isTransient = false;
        m_lastBlinkMs = millis();
        m_blinkPhase = true;
    } else {
        updateFromSystem();
    }
}

void RgbLedManager::setState(RgbStatusState state) {
    m_currentState = state;
    m_isTransient = false;
    m_lastBlinkMs = millis();
}

void RgbLedManager::updateFromSystem() {
    if (m_isTransient) return;

    SystemState state;
    if (g_stateMutex && xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
        state = g_systemState;
        xSemaphoreGive(g_stateMutex);
    } else {
        return;
    }

    if (state.hasActiveAlert) {
        m_currentState = LED_STATE_ALERT;
    } else if (!state.wifiConnected) {
        m_currentState = LED_STATE_CONNECTING_WIFI;
    } else if (!state.serverConnected) {
        m_currentState = LED_STATE_CONNECTING_POS;
    } else if (state.state == STATE_PRINTING) {
        m_currentState = LED_STATE_PRINTING;
    } else {
        m_currentState = LED_STATE_READY_ONLINE;
    }
}

void RgbLedManager::update() {
    uint32_t now = millis();

    if (m_isTransient) {
        if (now > m_transientExpiryMs) {
            m_isTransient = false;
            updateFromSystem();
        }
    } else {
        static uint32_t lastSysCheck = 0;
        if (now - lastSysCheck > 500) {
            lastSysCheck = now;
            updateFromSystem();
        }
    }

    switch (m_currentState) {
        case LED_STATE_PRINTING:
            if (now - m_lastBlinkMs > 150) {
                m_lastBlinkMs = now;
                m_blinkPhase = !m_blinkPhase;
            }
            if (m_blinkPhase) {
                writeColor(0, 15, 15);
            } else {
                writeColor(0, 0, 0);
            }
            break;

        case LED_STATE_PRINT_COMPLETED:
            if (now - m_lastBlinkMs > 180) {
                m_lastBlinkMs = now;
                m_blinkPhase = !m_blinkPhase;
            }
            if (m_blinkPhase) {
                writeColor(0, 25, 2);
            } else {
                writeColor(0, 0, 0);
            }
            break;

        case LED_STATE_BOOTING:
        case LED_STATE_READY_ONLINE:
        case LED_STATE_CONNECTING_WIFI:
        case LED_STATE_CONNECTING_POS:
        case LED_STATE_PRINTER_OFFLINE:
        case LED_STATE_PRINT_FAILED:
        case LED_STATE_ALERT:
        default:
            // Disturbing LED is kept completely OFF
            writeColor(0, 0, 0);
            break;
    }
}
