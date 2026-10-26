#ifndef NERAM_CONFIG_H
#define NERAM_CONFIG_H

#include <Arduino.h>

// ==============================================================================
// NERAM POS - ESP32-S3-N16R8 HARDWARE PRINT BRIDGE CONFIGURATION
// ==============================================================================

// Firmware Version & Hardware Signature
#define FIRMWARE_VERSION       "v2.0.0-esp32"
#define HARDWARE_MODEL         "ESP32-S3-N16R8"
// Per-device bridge ID prefix; the MAC suffix is appended at runtime unless BRIDGE_ID is set in .env
#define BRIDGE_DEVICE_ID_PREFIX "NERAM-ESP32"

// ------------------------------------------------------------------------------
// 1. Build-time configuration (injected from neram-print-bridge/.env by load_env.py)
//    There are deliberately NO fallbacks: a build without them must fail.
// ------------------------------------------------------------------------------
#ifndef BACKEND_BASE_URL
#error "BACKEND_BASE_URL missing - build through PlatformIO (load_env.py injects it from .env)"
#endif
#ifndef BACKEND_AUTH_TOKEN
#error "BACKEND_AUTH_TOKEN missing - build through PlatformIO (load_env.py injects it from .env)"
#endif
#ifndef BACKEND_TENANT_CODE
#error "BACKEND_TENANT_CODE missing - build through PlatformIO (load_env.py injects it from .env)"
#endif
#ifndef DEFAULT_WIFI_SSID
#error "DEFAULT_WIFI_SSID missing - build through PlatformIO (load_env.py injects it from .env)"
#endif
#ifndef DEFAULT_WIFI_PASS
#error "DEFAULT_WIFI_PASS missing - build through PlatformIO (load_env.py injects it from .env)"
#endif
// Printer IPs are managed in the POS UI and synced at runtime. The .env values are optional boot hints.
#ifndef DEFAULT_BILL_PRINTER_HOST
#define DEFAULT_BILL_PRINTER_HOST ""
#endif
#ifndef DEFAULT_KOT_PRINTER_HOST
#define DEFAULT_KOT_PRINTER_HOST ""
#endif
#ifndef DEFAULT_BILL_PRINTER_PORT
#define DEFAULT_BILL_PRINTER_PORT 9100
#endif
#ifndef DEFAULT_KOT_PRINTER_PORT
#define DEFAULT_KOT_PRINTER_PORT 9100
#endif
#ifndef DEFAULT_JUICE_KOT_PRINTER_HOST
#define DEFAULT_JUICE_KOT_PRINTER_HOST ""
#endif
#ifndef DEFAULT_JUICE_KOT_PRINTER_PORT
#define DEFAULT_JUICE_KOT_PRINTER_PORT 9100
#endif
#ifndef BRIDGE_ID_OVERRIDE
#define BRIDGE_ID_OVERRIDE ""
#endif

// WiFi
#define WIFI_CONNECT_TIMEOUT_MS 20000

// NTP (needed for TLS certificate validation)
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.cloudflare.com"
#define NTP_SERVER_3 "time.google.com"
#define NTP_VALID_AFTER_EPOCH 1700000000UL   // any clock earlier than Nov 2023 means "not synced"

// ------------------------------------------------------------------------------
// 2. Cloud polling / reporting
// ------------------------------------------------------------------------------
#define LONG_POLL_TIMEOUT_SEC  15       // Server-side long-poll hold (server: 15s)
#define POLL_HTTP_TIMEOUT_MS   (LONG_POLL_TIMEOUT_SEC * 1000 + 8000)
#define API_HTTP_TIMEOUT_MS    4000     // Short calls: ack / complete / fail / heartbeat / config
#define HTTP_CONNECT_TIMEOUT_MS 6000
#define TLS_HANDSHAKE_TIMEOUT_S 10
#define HEARTBEAT_INTERVAL_MS  20000
#define CONFIG_SYNC_INTERVAL_MS 60000
#define PRINTER_CHECK_INTERVAL_MS 30000
#define PENDING_REPORT_RETRY_MS 5000

// Poll failure backoff (exponential, capped)
#define POLL_BACKOFF_MIN_MS    1000
#define POLL_BACKOFF_MAX_MS    30000
#define POLL_MIN_CYCLE_MS      1000     // Guard against a server that answers 204 instantly

// Hard limits (protect heap from a misbehaving / spoofed server)
#define MAX_RESPONSE_BYTES     (384 * 1024)   // Whole HTTP body (base64 JSON)
#define MAX_PAYLOAD_BYTES      (256 * 1024)   // Decoded ESC/POS bytes
#define MAX_JOB_ID_LEN         47

// Report retry policy
#define REPORT_RETRIES         4
#define REPORT_BACKOFF_MS      300

// ------------------------------------------------------------------------------
// 3. Local print queue
// ------------------------------------------------------------------------------
#define PRINT_QUEUE_LEN        4        // Per printer
#define JOB_REGISTRY_SIZE      32       // Recently seen job IDs (dedupe on redelivery)
#define PENDING_REPORT_SLOTS   8        // Completed jobs whose /complete call is still unreported
#define JOB_STALE_MS           45000    // Server claim lease is 45s (+ack extension); older jobs are returned, not printed

// ------------------------------------------------------------------------------
// 4. Thermal printers (ESC/POS over TCP)
// ------------------------------------------------------------------------------
// Three printers: Bill, Kitchen KOT and Juice KOT. Addresses are synced from the cloud POS (/api/printer/bridge/config). The values
// from .env are only boot-time fallbacks. Targets must be private-LAN IPv4 + port range below.
#define PRINTER_PORT_MIN       9100
#define PRINTER_PORT_MAX       9109
#define PRINTER_SOCKET_TIMEOUT_MS 3000
#define PRINTER_CONNECT_ATTEMPTS  3
#define PRINTER_PING_TIMEOUT_MS   250
#define PRINTER_CHUNK_BYTES    1460     // One TCP segment (MSS)
#define PRINTER_CLOSE_LINGER_MS 20      // Let lwip drain the send buffer before FIN

// ------------------------------------------------------------------------------
// 5. 1.3" / 0.96" I2C OLED Display Configuration (SSD1306 / SH1106)
// ------------------------------------------------------------------------------
// Most 0.96" and 1.3" OLED modules use the SSD1306 controller.
// SSD1306 driver enables the internal charge pump DC-DC booster (0x8D 0x14)
// which turns on both SSD1306 and SH1106 screens.
// If you specifically have an SH1106 and notice 2px text offset, enable OLED_USE_SH1106.
// Master switch for the OLED. 0 = display is never probed or drawn (no I2C use, no
// [DISPLAY] log lines); the status LED keeps working. Set to 1 once the OLED is wired.
#define OLED_ENABLED           0

#define OLED_USE_SH1106        1
// #define OLED_USE_SSD1306       1

#define OLED_I2C_ADDR          0x3C     // Default I2C address (some boards use 0x3D)
#define OLED_SDA_PIN           17       // Current OLED wiring: SDA to GPIO17
#define OLED_SCL_PIN           18       // Current OLED wiring: SCL to GPIO18
#define OLED_I2C_FREQ          100000   // 100kHz Standard I2C bus for high noise immunity

// Screen rotation / cycle timing
#define SCREEN_CYCLE_TIME_MS   3500     // Automatically cycles pages every 3.5s
#define OLED_REFRESH_FPS       15       // 15 FPS refresh rate

// ------------------------------------------------------------------------------
// 6. System Task Sizes & Priorities (FreeRTOS Dual-Core Architecture)
// ------------------------------------------------------------------------------
#define NETWORK_TASK_CORE      0        // Core 0: Wi-Fi, HTTP, TCP printing socket
#define DISPLAY_TASK_CORE      1        // Core 1: OLED rendering and user status loop
#define NETWORK_TASK_STACK     16384    // Poll task (TLS handshake needs a big stack)
#define PRINT_TASK_STACK       16384    // One per printer
#define MAINT_TASK_STACK       16384    // Heartbeat / config / health checks
#define TASK_WDT_TIMEOUT_S     120
#define DISPLAY_TASK_STACK     8192     // 8KB stack in internal RAM

#endif // NERAM_CONFIG_H
