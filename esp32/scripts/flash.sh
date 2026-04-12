#!/usr/bin/env bash
# flash.sh — Flash FlipperClaw firmware to ESP32-S3
set -euo pipefail

PORT="${1:-}"

if [ -z "${PORT}" ]; then
    echo "Usage: ./scripts/flash.sh <serial_port>"
    echo "  e.g. ./scripts/flash.sh /dev/ttyUSB0"
    echo "  e.g. ./scripts/flash.sh /dev/cu.usbserial-0001"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="$(dirname "${SCRIPT_DIR}")"
IDF_DIR="${HOME}/esp/esp-idf"

# Source IDF environment
if [ -z "${IDF_PATH:-}" ]; then
    if [ -f "${IDF_DIR}/export.sh" ]; then
        # shellcheck disable=SC1091
        . "${IDF_DIR}/export.sh"
    else
        echo "[flash] ERROR: ESP-IDF not found. Run ./scripts/setup_idf.sh first"
        exit 1
    fi
fi

cd "${ESP32_DIR}"

if [ ! -d "build" ]; then
    echo "[flash] No build directory found — run ./scripts/build.sh first"
    exit 1
fi

echo "[flash] Flashing to ${PORT}..."
idf.py -p "${PORT}" flash

echo ""
echo "[flash] Flash complete. Opening monitor (Ctrl+] to exit)..."
idf.py -p "${PORT}" monitor
