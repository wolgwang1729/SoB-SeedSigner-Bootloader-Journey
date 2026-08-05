#!/usr/bin/env bash
set -eo pipefail

PORT="${ESPPORT:-/dev/ttyACM0}"
BAUD="${ESPBAUD:-115200}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOOTLOADER_DIR="$SCRIPT_DIR/seedsigner_bootloader_p4_stateless_os"
PAYLOAD_BIN="${MICROPYTHON_BIN:-$HOME/Desktop/seedsigner-micropython-builder/build/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/micropython.bin}"

if ! command -v idf.py >/dev/null; then
    . $HOME/esp/esp-idf-v5.5/export.sh || . $HOME/esp/esp-idf/export.sh
fi

echo "=== Flashing Bootloader ==="
(cd "$BOOTLOADER_DIR" && idf.py build && idf.py -p "$PORT" -b "$BAUD" flash)

echo "=== Flashing MicroPython Payload ==="
esptool.py --chip esp32p4 --port "$PORT" --baud "$BAUD" write_flash 0x140000 "$PAYLOAD_BIN"

echo "=== Starting Monitor ==="
(cd "$BOOTLOADER_DIR" && idf.py -p "$PORT" monitor)
