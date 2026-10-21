#ifndef NERAM_DISPLAY_MANAGER_H
#define NERAM_DISPLAY_MANAGER_H

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "system_state.h"

class DisplayManager {
public:
    DisplayManager();
    bool begin();
    void update();
    void setScreen(DisplayScreenMode mode);
    void nextScreen();

private:
    U8G2* m_u8g2;
    DisplayScreenMode m_currentScreen;
    uint32_t m_lastCycleTime;
    bool m_displayFound;
    uint32_t m_lastProbeTime;
    uint8_t m_activeSda;
    uint8_t m_activeScl;
    uint8_t m_activeAddr;

    bool probeI2C(uint8_t sdaPin, uint8_t sclPin, uint8_t addr);
    bool tryDetectDisplay();

    void drawStatusBar(const SystemState& state);
    void drawScreenServer(const SystemState& state);
    void drawScreenESP32(const SystemState& state);
    void drawScreenPrinters(const SystemState& state);
    void drawScreenPrinting(const SystemState& state);
    void drawScreenAlert(const SystemState& state);
};

extern DisplayManager g_display;

#endif // NERAM_DISPLAY_MANAGER_H
