# ARVI POS - Thermal Printer Cloud Bridge

Firmware for an ESP32-S3 microcontroller (ESP32-S3-N16R8, 16 MB flash, 8 MB PSRAM) that securely communicates with the ARVI POS backend over **HTTPS** and dispatches print jobs to local area network (LAN) thermal printers (ESC/POS protocol, TCP port 9100).

Equipped with an SSD1306/SH1106 OLED display and an onboard WS2812 RGB LED for real-time visual system telemetry.

---

## Architecture & How It Works

```
networkTask          long-poll /api/printer/bridge/poll (TLS) -> decode -> per-printer queue -> ack receipt
PrintBill/Kot/Juice  dedicated worker per printer (Juice & Kitchen prioritized ahead of Bill):
                     claim -> TCP raw socket stream -> complete | fail report
maintenance          config sync (60s), printer health probes (30s), heartbeat (20s), lease renewals
```

- **Security**: TLS encryption with pinned certificates, NTP-synchronized time validation, print destinations strictly restricted to private LAN IPv4 addresses, and cryptographic bridge token authentication (`X-Bridge-Token`).
- **Reliability**: Lease-based dispatch protocol (claim -> ack -> sent -> complete/fail) with automatic redelivery deduplication to prevent double printing on network drops.
- **Performance**: Persistent HTTPS keep-alive, TCP socket streaming without Nagle latency delays, and parallel dispatch to Bill, Kitchen KOT, and Juice KOT printers.

---

## Configuration

All configuration is provided via environment variables (never committed to git):

| Key | Description | Example / Note |
|---|---|---|
| `BACKEND_BASE_URL` | Base URL of the ARVI POS backend | `https://pos.yourcafe.com` (HTTPS required for production) |
| `PRINTER_BRIDGE_TOKEN` | Shared secret token matching backend's `PRINTER_BRIDGE_TOKEN` | `<your_bridge_secret_token>` |
| `WIFI_SSID` | Local Wi-Fi network SSID | `<your_wifi_ssid>` |
| `WIFI_PASSWORD` | Local Wi-Fi password | `<your_wifi_password>` |
| `BRIDGE_ID` | Identifier for the bridge device | `bridge-counter-01` |
| `BILL_PRINTER_HOST` | Fallback Bill thermal printer IP | `192.168.1.200` |
| `BILL_PRINTER_PORT` | Bill printer raw TCP port | `9100` |
| `KITCHEN_PRINTER_HOST` | Fallback Kitchen KOT printer IP | `192.168.1.201` |
| `KITCHEN_PRINTER_PORT` | Kitchen printer raw TCP port | `9100` |
| `JUICE_PRINTER_HOST` | Fallback Juice KOT printer IP | `192.168.1.202` |
| `JUICE_PRINTER_PORT` | Juice printer raw TCP port | `9100` |

Copy `.env.example` to `.env` and fill in your site-specific values:

```bash
cp .env.example .env
```

`load_env.py` verifies configuration at build time and prevents building if required variables are missing.

---

## Flashing the Hardware

```bash
# Build and flash using PlatformIO wrapper
./flash-esp32.sh

# Or build without flashing
./flash-esp32.sh --build-only
```

---

## Hardware Specifications

- **Microcontroller**: ESP32-S3-WROOM-1 / ESP32-S3-DevKitC-1 (16MB Flash, 8MB Octal PSRAM)
- **Display**: 0.96" or 1.3" I2C OLED (SH1106 / SSD1306, Address `0x3C`, `SDA=8`, `SCL=9`)
- **Status Indicator**: Onboard WS2812 RGB LED (Blue = Connecting, Green = Healthy/Idle, Orange = Printing, Red = Alert/Error)
