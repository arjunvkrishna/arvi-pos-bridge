# Neram POS - ESP32-S3 Print Bridge

Firmware for an ESP32-S3-N16R8 (16 MB flash, 8 MB PSRAM) that long-polls the Neram POS backend over **HTTPS** and prints jobs to LAN thermal printers (ESC/POS, TCP 9100). An SSD1306/SH1106 OLED and the onboard RGB LED show live status.

## Configuration (env files in this folder)

All settings come from env files in **this folder** (never the backend's root `.env`):

| File | Used by |
|---|---|
| `.env` | `./flash-esp32.sh` (default) |
| `.env.stg` | `./flash-esp32.sh stg` |
| `.env.prod` | `./flash-esp32.sh prod` |

Copy `.env.example`, fill it in. Keys: `BACKEND_BASE_URL` (https only: `https://stg.neram.store` / `https://pos.neram.store`), `PRINTER_BRIDGE_TOKEN`, `WIFI_SSID`, `WIFI_PASSWORD`, optional `BRIDGE_ID`. **Printer IPs are not needed here**: Bill, Kitchen KOT and Juice KOT addresses are managed in the POS UI and synced to the bridge (`*_PRINTER_HOST/PORT` keys are optional boot hints). Use a different token per environment. The files are gitignored.

`load_env.py` runs at build time and **fails the build** if a key is missing or the URL is not `https://` (lab backends: `ALLOW_INSECURE_HTTP=1`). There are no hidden fallbacks.

## Flash

```bash
./neram-print-bridge/flash-esp32.sh stg   # (or ./flash-esp32.sh from the repo root, or ./flash.sh here); shows URL/SSID/printers, asks to confirm
./flash-esp32.sh prod       # must type "prod" to continue
./flash-esp32.sh --build-only
```
Options: `--port /dev/ttyACM0`, `--no-monitor`, `--yes` (not allowed for prod). Boot log should show `[WIFI] Connected`, `[NTP] Clock synchronized`, `[CLOUD] Bridge ID ...`.

## How it works

```
networkTask   long-poll /api/printer/bridge/poll (persistent TLS) -> decode -> per-printer queue -> ack-receipt
PrintBill/Kot/Juice one worker per printer (kitchen + juice ahead of bill): sent -> TCP stream -> complete | fail
maintenance   config sync (60s), printer probes (30s), heartbeat (20s), retry unreported completions
```

- **Security:** TLS with pinned roots (ISRG X1/X2, `include/root_ca.h`), NTP-validated clock, print targets limited to private LAN IPv4 on ports 9100-9109, response/payload size caps, job-ID validation.
- **Reliability:** follows the backend lease protocol (claim -> ack -> sent -> complete/fail, `ambiguous` on mid-write failures), dedupes redelivered jobs (never reprints), retries reports with backoff, exponential poll backoff, task watchdog (120 s).
- **Speed:** keep-alive HTTPS connections, no per-job TLS handshake, 1460-byte TCP chunks with Nagle off, bill, kitchen KOT and juice KOT printed in parallel, heartbeat/probes never block printing, Wi-Fi power save off.

## Hardware

OLED I2C `SDA=8 SCL=9` (0x3C, SH1106 by default, see `config.h`), onboard WS2812 RGB LED. Partition table `default_16MB.csv`, PSRAM `qio_opi`.

## Not yet done / limitations

- The job queue is in RAM: a power cut loses queued (not yet printed) jobs; the server re-dispatches them after the claim lease expires.
- No ESC/POS paper-out/offline status query before printing.
- Root CAs are pinned in firmware; if the backend moves to a different CA family, update `include/root_ca.h` and reflash.
