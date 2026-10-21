#ifndef NERAM_RGB_LED_MANAGER_H
#define NERAM_RGB_LED_MANAGER_H

#include <Arduino.h>

// ==============================================================================
// NERAM POS - ESP32-S3 ONBOARD RGB LED STATUS MANAGER
// WS2812 NeoPixel Driver with multi-scenario color cues & animations
// ==============================================================================

enum RgbStatusState {
    LED_STATE_BOOTING,          // Soft White steady
    LED_STATE_CONNECTING_WIFI,  // Orange / Amber blinking
    LED_STATE_CONNECTING_POS,   // Blue blinking (WiFi OK, awaiting POS cloud)
    LED_STATE_READY_ONLINE,     // Steady Soothing Green (WiFi OK + POS OK + Printers OK)
    LED_STATE_PRINTER_OFFLINE,  // Steady Yellow / Amber (POS OK, but printer unreachable)
    LED_STATE_PRINTING,         // Cyan rapid strobe / pulsing (Job in progress)
    LED_STATE_PRINT_COMPLETED,  // Bright Green celebration flash
    LED_STATE_PRINT_FAILED,     // Fast Red flash (Print job socket error)
    LED_STATE_ALERT             // Red pulsing / alert (Critical error)
};

class RgbLedManager {
public:
    RgbLedManager();
    void begin();
    void update(); // Called from Core 1 display task

    // Event triggers for real-time actions (thread-safe, updates state flags)
    void triggerPrintSuccess();
    void triggerPrintFailed();
    void triggerPrinting(bool active);
    void setState(RgbStatusState state);
    void updateFromSystem();

    // Direct color control with change-detection (avoids flooding RMT)
    void writeColor(uint8_t r, uint8_t g, uint8_t b);

private:
    volatile RgbStatusState m_currentState;
    uint32_t m_lastBlinkMs;
    bool m_blinkPhase;
    uint32_t m_transientExpiryMs;
    volatile bool m_isTransient;

    uint8_t m_currentR;
    uint8_t m_currentG;
    uint8_t m_currentB;
};

extern RgbLedManager g_rgbLed;

#endif // NERAM_RGB_LED_MANAGER_H
