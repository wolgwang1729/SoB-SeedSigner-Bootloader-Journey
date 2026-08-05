#!/usr/bin/env bash
set -eo pipefail

PORT="${ESPPORT:-/dev/ttyACM0}"
BAUD="${ESPBAUD:-921600}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOOTLOADER_DIR="$SCRIPT_DIR/seedsigner_bootloader_p4_stateless_os"
PAYLOAD_BIN="${MICROPYTHON_BIN:-$HOME/Desktop/seedsigner-micropython-builder/build/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/micropython.bin}"

if ! command -v idf.py >/dev/null; then
    . $HOME/esp/esp-idf-v5.5/export.sh || . $HOME/esp/esp-idf/export.sh
fi

echo "=== Building Bootloader ==="
(cd "$BOOTLOADER_DIR" && idf.py build)

BOOTLOADER_BIN="$BOOTLOADER_DIR/build/bootloader/bootloader.bin"
PARTITION_BIN="$BOOTLOADER_DIR/build/partition_table/partition-table.bin"
LOADER_BIN="$BOOTLOADER_DIR/build/seedsigner_secure_loader.bin"

echo "=== Flashing Everything ==="
esptool.py --chip esp32p4 --port "$PORT" --baud "$BAUD" \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_freq 40m --flash_size 8MB \
    0x2000 "$BOOTLOADER_BIN" \
    0x20000 "$PARTITION_BIN" \
    0x30000 "$LOADER_BIN" \
    0x140000 "$PAYLOAD_BIN"

echo "=== Starting Monitor ==="
(cd "$BOOTLOADER_DIR" && idf.py -p "$PORT" monitor)
