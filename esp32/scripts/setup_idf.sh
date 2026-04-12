#!/usr/bin/env bash
# setup_idf.sh — Clone and install ESP-IDF v5.5 for FlipperClaw
set -euo pipefail

IDF_DIR="${HOME}/esp/esp-idf"
IDF_TAG="v5.5"

echo "[setup_idf] Installing ESP-IDF ${IDF_TAG} to ${IDF_DIR}"

if [ -d "${IDF_DIR}" ]; then
    echo "[setup_idf] Directory exists — pulling latest ${IDF_TAG}"
    git -C "${IDF_DIR}" fetch origin tag "${IDF_TAG}" --no-tags
    git -C "${IDF_DIR}" checkout "${IDF_TAG}"
    git -C "${IDF_DIR}" submodule update --init --recursive
else
    mkdir -p "$(dirname "${IDF_DIR}")"
    git clone -b "${IDF_TAG}" --recursive \
        https://github.com/espressif/esp-idf.git "${IDF_DIR}"
fi

echo "[setup_idf] Running install.sh for esp32s3 target..."
"${IDF_DIR}/install.sh" esp32s3

echo ""
echo "[setup_idf] Done. To activate the IDF environment run:"
echo "    . ${IDF_DIR}/export.sh"
echo ""
echo "Or add to your shell profile:"
echo "    alias get_idf='. ${IDF_DIR}/export.sh'"
