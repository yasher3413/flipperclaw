#!/usr/bin/env bash
# build.sh — Build FlipperClaw ESP32-S3 firmware
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="$(dirname "${SCRIPT_DIR}")"
IDF_DIR="${HOME}/esp/esp-idf"

# Source IDF environment
if [ -z "${IDF_PATH:-}" ]; then
    if [ -f "${IDF_DIR}/export.sh" ]; then
        echo "[build] Sourcing ESP-IDF from ${IDF_DIR}"
        # shellcheck disable=SC1091
        . "${IDF_DIR}/export.sh"
    else
        echo "[build] ERROR: ESP-IDF not found at ${IDF_DIR}"
        echo "[build] Run ./scripts/setup_idf.sh first"
        exit 1
    fi
fi

cd "${ESP32_DIR}"

# Check secrets file exists
if [ ! -f "main/fc_secrets.h" ]; then
    echo "[build] ERROR: main/fc_secrets.h not found"
    echo "[build] Copy main/fc_secrets.h.example to main/fc_secrets.h and fill in your credentials"
    exit 1
fi

echo "[build] Setting target: esp32s3"
idf.py set-target esp32s3

echo "[build] Building firmware..."
idf.py build

echo ""
echo "[build] Build complete."
echo "[build] Firmware: ${ESP32_DIR}/build/flipperclaw.bin"
echo "[build] Flash with: ./scripts/flash.sh <port>"
