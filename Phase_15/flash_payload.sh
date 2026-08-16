#!/usr/bin/env bash
# ==============================================================================
# Flash-Payload Helper Script — Phase 15 (dev/bring-up only)
#
# Flashes the RAW hello-world shim image into the 'payload' data partition at
# 0x220000. With no SD card attached, the loader falls back to booting this
# raw 0xE9 image from flash (Specter verification SKIPPED) — a stepping stone
# to bring up the loader -> JMP -> PSRAM execution chain on silicon before the
# S3's SD wiring is soldered. The production path (SD card) is untouched.
#
# Usage: ./flash_payload.sh
#   ESPPORT  serial port        (default /dev/ttyACM0)
#   ESPBAUD  baud               (default 921600)
# ==============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log_info()    { echo -e "\033[1;34m[INFO]\033[0m $*"; }
log_success() { echo -e "\033[1;32m[PASS]\033[0m $*"; }
log_error()   { echo -e "\033[1;31m[FAIL]\033[0m $*" >&2; }
log_warn()    { echo -e "\033[1;33m[WARN]\033[0m $*"; }
log_step()    { echo -e "\n\033[1;35m=== $* ===\033[0m"; }

# ------------------------------------------------------------------------------
# ESP-IDF environment
# ------------------------------------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    for candidate in \
        "${IDF_PATH:-__none__}/export.sh" \
        "$HOME/esp/esp-idf-v5.5/export.sh" \
        "$HOME/esp/esp-idf/export.sh"; do
        [ -f "$candidate" ] && { . "$candidate" >/dev/null 2>&1 || true; break; }
    done
fi
if ! command -v esptool.py >/dev/null 2>&1; then
    log_error "esptool.py not found. Source ESP-IDF export.sh first."
    exit 1
fi

if [ -n "${ESPPORT:-}" ]; then
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
BAUD="${ESPBAUD:-921600}"

# ------------------------------------------------------------------------------
# Locate the raw payload image
# ------------------------------------------------------------------------------
log_step "Flash raw payload to the 'payload' partition @ 0x220000"
PAYLOAD_BIN="$SCRIPT_DIR/build/hello_world_esp32s3_raw.bin"
[ -f "$PAYLOAD_BIN" ] || PAYLOAD_BIN="$SCRIPT_DIR/hello_world_esp32s3_stock_shim/build/hello_world.bin"
if [ ! -f "$PAYLOAD_BIN" ] || [ ! -s "$PAYLOAD_BIN" ]; then
    log_error "Raw payload image not found: $PAYLOAD_BIN"
    log_error "Build it first: $SCRIPT_DIR/build_payload.sh"
    exit 1
fi
log_info "  Raw payload: $PAYLOAD_BIN ($(stat -c%s "$PAYLOAD_BIN") bytes)"

if [ ! -e "$PORT" ]; then
    log_error "Port $PORT not found. Connect the board or set ESPPORT."
    exit 1
fi

esptool.py --chip esp32s3 --port "$PORT" --baud "$BAUD" \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x220000 "$PAYLOAD_BIN"

log_success "Raw payload flashed to 'payload' @ 0x220000."
log_warn "With no SD card attached, the loader boots this image directly "
log_warn "(dev path, no Specter verification). Reset the board to boot it."
exit 0
