#!/usr/bin/env bash
# ==============================================================================
# Neram POS - ESP32-S3 Print Bridge: build + flash + serial monitor
# ==============================================================================
# All configuration (backend URL, token, Wi-Fi, printers) comes from an env file
# inside neram-print-bridge/ (next to this script) - never from the backend's root .env.
#
#   ./flash-esp32.sh                 # uses neram-print-bridge/.env
#   ./flash-esp32.sh stg             # uses neram-print-bridge/.env.stg
#   ./flash-esp32.sh prod            # uses neram-print-bridge/.env.prod (asks you to type "prod")
#   ./flash-esp32.sh path/to/file    # any other env file
#
# Options: --port /dev/ttyACM0   --yes (skip confirmation, not allowed for prod)
#          --no-monitor          --build-only
# ==============================================================================
set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BLUE='\033[0;34m'; NC='\033[0m'
die() { echo -e "${RED}[ERROR] $*${NC}" >&2; exit 1; }

BRIDGE_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"   # this script lives in neram-print-bridge/
ROOT_DIR="$(cd "$BRIDGE_DIR/.." && pwd)"

TARGET=""; PORT=""; ASSUME_YES=0; MONITOR=1; BUILD_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --port)       PORT="${2:-}"; [ -n "$PORT" ] || die "--port needs a device path"; shift 2 ;;
        --yes|-y)     ASSUME_YES=1; shift ;;
        --no-monitor) MONITOR=0; shift ;;
        --build-only) BUILD_ONLY=1; shift ;;
        -h|--help)    sed -n '2,17p' "$0"; exit 0 ;;
        -*)           die "Unknown option: $1" ;;
        *)            [ -z "$TARGET" ] || die "Only one environment may be given"; TARGET="$1"; shift ;;
    esac
done

# ---- 1. Resolve the env file --------------------------------------------------
case "$TARGET" in
    "")   ENV_FILE="$BRIDGE_DIR/.env";      LABEL="default (.env)" ;;
    stg)  ENV_FILE="$BRIDGE_DIR/.env.stg";  LABEL="STAGING" ;;
    prod) ENV_FILE="$BRIDGE_DIR/.env.prod"; LABEL="PRODUCTION" ;;
    *)    ENV_FILE="$(readlink -f "$TARGET" 2>/dev/null || echo "$TARGET")"; LABEL="custom ($TARGET)" ;;
esac
[ -f "$ENV_FILE" ] || die "Env file not found: $ENV_FILE
       Copy .env.example to it and fill in the values."

get() { grep -E "^$1=" "$ENV_FILE" | head -n1 | cut -d= -f2- | sed -e "s/^[\"']//" -e "s/[\"']\$//"; }
URL="$(get BACKEND_BASE_URL)"; SSID="$(get WIFI_SSID)"

echo -e "\n${BLUE}==============================================================================${NC}"
echo -e "${BLUE}  NERAM POS - ESP32-S3 PRINT BRIDGE FLASHER${NC}"
echo -e "${BLUE}==============================================================================${NC}"
echo -e "  Target     : ${YELLOW}${LABEL}${NC}"
echo -e "  Env file   : ${ENV_FILE#$ROOT_DIR/}"
echo -e "  Backend    : ${URL:-<missing>}"
echo -e "  Wi-Fi SSID : ${SSID:-<missing>}"
echo -e "  Printers   : IPs come from the POS UI (Bill / Kitchen KOT / Juice KOT)"
echo -e "  Bridge ID  : $(get BRIDGE_ID | sed 's/^$/(auto from MAC)/')"
echo -e "  Token      : $( [ -n "$(get PRINTER_BRIDGE_TOKEN)" ] && echo "set (hidden)" || echo "<missing>")\n"

# Sanity: the env name must match the URL so a stg board can never get prod settings (and vice versa)
case "$TARGET" in
    stg)  [[ "$URL" == *"stg."* ]]  || die ".env.stg points at '$URL' - expected a stg.* host" ;;
    prod) [[ "$URL" == *"stg."* ]]  && die ".env.prod points at a staging host: $URL"
          [[ "$URL" == "https://pos.neram.store"* ]] || echo -e "${YELLOW}[WARN] prod URL is not https://pos.neram.store${NC}" ;;
esac

if [ "$TARGET" = "prod" ]; then
    [ "$ASSUME_YES" -eq 0 ] || die "--yes is not allowed for prod; confirm interactively"
    read -r -p "Flashing PRODUCTION firmware. Type 'prod' to continue: " ANSWER
    [ "$ANSWER" = "prod" ] || die "Aborted"
elif [ "$ASSUME_YES" -eq 0 ]; then
    read -r -p "Flash this configuration? [y/N]: " -n 1 ANSWER; echo
    [[ "$ANSWER" =~ ^[Yy]$ ]] || die "Aborted"
fi

# ---- 2. PlatformIO ---------------------------------------------------------------
PIO_BIN=""
if command -v pio >/dev/null 2>&1; then PIO_BIN="pio"
elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then PIO_BIN="$HOME/.platformio/penv/bin/pio"; fi

if [ "$BUILD_ONLY" -eq 1 ]; then
    echo -e "${YELLOW}[1/2] Building (no upload)...${NC}"
    [ -n "$PIO_BIN" ] || die "Build-only needs a local PlatformIO install"
    (cd "$BRIDGE_DIR" && ENV_FILE="$ENV_FILE" "$PIO_BIN" run)
    echo -e "${GREEN}[SUCCESS] Build OK${NC}"
    exit 0
fi

# ---- 3. Serial port --------------------------------------------------------------
echo -e "${YELLOW}[1/3] Looking for the ESP32-S3 USB serial device...${NC}"
if [ -z "$PORT" ]; then
    mapfile -t PORTS < <(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true)
    if [ "${#PORTS[@]}" -eq 0 ]; then
        die "No ESP32 detected. Plug it in with a USB-C DATA cable (use the UART/COM port on two-port boards)."
    elif [ "${#PORTS[@]}" -eq 1 ]; then
        PORT="${PORTS[0]}"
    else
        [ "$ASSUME_YES" -eq 0 ] || die "Several serial devices (${PORTS[*]}); pass --port"
        echo "Several serial devices found:"; select P in "${PORTS[@]}"; do PORT="$P"; break; done
        [ -n "$PORT" ] || die "No port selected"
    fi
fi
[ -e "$PORT" ] || die "Port $PORT does not exist"
echo -e "${GREEN}[FOUND] $PORT${NC}"

fix_permissions() {
    [ -w "$PORT" ] && return 0
    if command -v docker >/dev/null 2>&1; then
        docker run --rm --privileged -v /dev:/dev alpine chmod 666 "$PORT" 2>/dev/null || true
    fi
    [ -w "$PORT" ] || echo -e "${YELLOW}[WARN] $PORT is not writable. Permanent fix: sudo usermod -aG dialout \$USER (then re-login)${NC}"
}
fix_permissions

# ---- 4. Build + upload -----------------------------------------------------------
echo -e "\n${YELLOW}[2/3] Building and uploading firmware to $PORT...${NC}"
if [ -n "$PIO_BIN" ]; then
    echo -e "${BLUE}Using local PlatformIO: $PIO_BIN${NC}"
    (cd "$BRIDGE_DIR" && ENV_FILE="$ENV_FILE" "$PIO_BIN" run --target upload --upload-port "$PORT")
else
    command -v docker >/dev/null 2>&1 || die "Neither PlatformIO nor Docker is available"
    case "$ENV_FILE" in
        "$ROOT_DIR"/*) ;;
        *) die "With Docker the env file must live inside this repository" ;;
    esac
    echo -e "${BLUE}Using Docker PlatformIO container...${NC}"
    docker run --rm --privileged \
        -v /dev:/dev -v "$ROOT_DIR":/workspace -v "$HOME/.platformio":/root/.platformio \
        -e ENV_FILE="/workspace/${ENV_FILE#$ROOT_DIR/}" \
        -w /workspace/neram-print-bridge python:3.12-slim \
        sh -c "pip install --no-cache-dir platformio >/dev/null 2>&1 && pio run --target upload --upload-port $PORT"
fi
fix_permissions   # The port re-enumerates after reset

echo -e "\n${GREEN}[SUCCESS] Firmware flashed ($LABEL)${NC}"
echo -e "${GREEN}Configuration loaded from ${ENV_FILE#$ROOT_DIR/}${NC}\n"

# ---- 5. Serial monitor -----------------------------------------------------------
if [ "$MONITOR" -eq 1 ]; then
    REPLY="y"
    [ "$ASSUME_YES" -eq 1 ] || read -r -p "Open the serial monitor to verify the boot log? [Y/n]: " -n 1 REPLY || true
    echo
    if [[ -z "$REPLY" || "$REPLY" =~ ^[Yy]$ ]]; then
        echo -e "${BLUE}Serial monitor (Ctrl+C to exit). Look for: [WIFI] Connected, [NTP] Clock synchronized, [CLOUD] Bridge ID...${NC}"
        if [ -n "$PIO_BIN" ]; then
            (cd "$BRIDGE_DIR" && "$PIO_BIN" device monitor --port "$PORT" --baud 115200)
        else
            docker run --rm -it --privileged -v /dev:/dev -v "$HOME/.platformio":/root/.platformio \
                python:3.12-slim sh -c "pip install --no-cache-dir platformio >/dev/null 2>&1 && pio device monitor --port $PORT --baud 115200"
        fi
    fi
fi
