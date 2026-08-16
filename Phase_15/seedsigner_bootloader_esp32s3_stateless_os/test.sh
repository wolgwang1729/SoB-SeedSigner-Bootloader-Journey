#!/usr/bin/env bash
# test.sh — Run the SeedSigner Stateless Bootloader pytest suite
# Usage: ./test.sh [PORT]
#   PORT defaults to /dev/ttyACM0

set -euo pipefail

if [ -n "${1:-}" ]; then
    PORT="$1"
elif [ -n "${ESPPORT:-}" ]; then
    PORT="$ESPPORT"
elif [ -e "/dev/ttyACM0" ]; then
    PORT="/dev/ttyACM0"
elif [ -e "/dev/ttyACM1" ]; then
    PORT="/dev/ttyACM1"
elif [ -e "/dev/ttyUSB0" ]; then
    PORT="/dev/ttyUSB0"
elif [ -e "/dev/ttyUSB1" ]; then
    PORT="/dev/ttyUSB1"
else
    PORT="/dev/ttyACM0"
fi
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source ~/esp/esp-idf-v5.5/export.sh

pytest -s --embedded-services esp,idf \
       --target esp32s3 \
       --port "$PORT" \
       "$SCRIPT_DIR/pytest_seedsigner_bootloader_esp32s3_stateless_os.py"
