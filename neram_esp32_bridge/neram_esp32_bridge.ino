// ==============================================================================
// NERAM POS - ESP32-S3-N16R8 PRINT BRIDGE (Arduino IDE 2.x Entrypoint)
// ==============================================================================
// Required Libraries (install via Arduino Library Manager):
// 1. ArduinoJson by Benoit Blanchon (Version 7.x)
// 2. U8g2 by oliver (Version 2.35.x)
//
// Arduino IDE Board Settings (Tools menu):
// - Board: "ESP32S3 Dev Module"
// - Flash Size: "16MB (128Mb)"
// - PSRAM: "OPI PSRAM"
// - Partition Scheme: "16M Flash (3MB APP/9.9MB FATFS)" or custom
// - Upload Speed: 921600
// - USB Mode: "Hardware Controls" or "CDC and JTAG"
// ==============================================================================

#include "../include/config.h"
#include "../include/system_state.h"
#include "../include/display_manager.h"
#include "../include/printer_client.h"
#include "../include/cloud_client.h"

// If compiling standalone in Arduino IDE without include paths,
// you can copy src/* and include/* directly into this folder or use PlatformIO.

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("[NERAM] ESP32-S3-N16R8 Print Bridge Sketch Started");
}

void loop() {
    delay(1000);
}
