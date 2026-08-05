#!/usr/bin/env bash
# ==============================================================================
# Build & Flash Script — Phase 11
# Target  : ESP32-P4 PSRAM Payload Loader
# Payload : raw ESP32 image (seed.bin) — loaded from 'payload' flash partition (0x140000)
# ==============================================================================
set -uo pipefail

# ------------------------------------------------------------------------------
# Logging
# ------------------------------------------------------------------------------
log_info()    { echo -e "\033[1;34m[INFO]\033[0m $*"; }
log_success() { echo -e "\033[1;32m[PASS]\033[0m $*"; }
log_warn()    { echo -e "\033[1;33m[WARN]\033[0m $*"; }
log_error()   { echo -e "\033[1;31m[FAIL]\033[0m $*" >&2; }
log_step()    { echo -e "\n\033[1;35m=== $* ===\033[0m"; }

# ------------------------------------------------------------------------------
# Path resolution
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -d "$SCRIPT_DIR/seedsigner_bootloader_p4_stateless_os" ]; then
    ROOT_DIR="$SCRIPT_DIR"
    BOOTLOADER_DIR="$SCRIPT_DIR/seedsigner_bootloader_p4_stateless_os"
    PAYLOAD_DIR="$SCRIPT_DIR/hello_world_esp32p4_stateless_payload"
elif [ -f "$SCRIPT_DIR/CMakeLists.txt" ]; then
    BOOTLOADER_DIR="$SCRIPT_DIR"
    ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
    PAYLOAD_DIR="$ROOT_DIR/hello_world_esp32p4_stateless_payload"
else
    log_error "Cannot locate project directories from $SCRIPT_DIR"
    exit 1
fi

# ------------------------------------------------------------------------------
# ESP-IDF environment
# ------------------------------------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    log_info "idf.py not in PATH — sourcing ESP-IDF..."
    for candidate in \
        "${IDF_PATH:-__none__}/export.sh" \
        "$HOME/esp/esp-idf-v5.5/export.sh" \
        "$HOME/esp/esp-idf/export.sh"; do
        [ -f "$candidate" ] && { . "$candidate" >/dev/null 2>&1 || true; break; }
    done
fi
if ! command -v idf.py >/dev/null 2>&1; then
    log_error "idf.py not found. Source ESP-IDF export.sh first."
    exit 1
fi

# ------------------------------------------------------------------------------
# Configuration
# ------------------------------------------------------------------------------
PORT="${ESPPORT:-/dev/ttyACM0}"
BAUD="${ESPBAUD:-921600}"

# ------------------------------------------------------------------------------
# Step 1: Build payload
# ------------------------------------------------------------------------------
log_step "Step 1: Build payload"
if [ ! -d "$PAYLOAD_DIR" ]; then
    log_error "Payload directory not found: $PAYLOAD_DIR"
    exit 1
fi
( cd "$PAYLOAD_DIR" && idf.py set-target esp32p4 && idf.py build )

# Detect payload binary name (project name may differ)
RAW_PAYLOAD_BIN=$(find "$PAYLOAD_DIR/build" -maxdepth 1 -name "*.bin" \
    ! -name "bootloader.bin" ! -name "partition-table.bin" | head -1)
if [ -z "$RAW_PAYLOAD_BIN" ] || [ ! -s "$RAW_PAYLOAD_BIN" ]; then
    log_error "Payload binary not found under $PAYLOAD_DIR/build"
    exit 1
fi
log_success "Payload binary: $RAW_PAYLOAD_BIN"

# ------------------------------------------------------------------------------
# Step 2: Verify payload binary
# ------------------------------------------------------------------------------
log_step "Step 2: Verify payload binary"
log_success "Payload binary ready for flashing: $(basename "$RAW_PAYLOAD_BIN") ($(wc -c < "$RAW_PAYLOAD_BIN") bytes)"

# ------------------------------------------------------------------------------
# Step 3: Build bootloader
# ------------------------------------------------------------------------------
log_step "Step 3: Build bootloader (seedsigner_bootloader_p4_stateless_os)"
( cd "$BOOTLOADER_DIR" && idf.py build )

BOOTLOADER_BIN="$BOOTLOADER_DIR/build/bootloader/bootloader.bin"
PARTITION_BIN="$BOOTLOADER_DIR/build/partition_table/partition-table.bin"
LOADER_BIN="$BOOTLOADER_DIR/build/seedsigner_secure_loader.bin"

for bin in "$BOOTLOADER_BIN" "$PARTITION_BIN" "$LOADER_BIN"; do
    if [ ! -f "$bin" ] || [ ! -s "$bin" ]; then
        log_error "Missing build artifact: $bin"
        exit 1
    fi
done
log_success "Bootloader build artifacts verified."

# ------------------------------------------------------------------------------
# Step 4: Flash everything
# ------------------------------------------------------------------------------
log_step "Step 4: Flash to $PORT at ${BAUD} baud"
if [ ! -e "$PORT" ]; then
    log_error "Port $PORT not found. Connect the board or set ESPPORT."
    exit 1
fi

# Flash the bootloader, partition table, factory app, and raw payload in a single command
esptool.py --chip esp32p4 --port "$PORT" --baud "$BAUD" \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_freq 40m --flash_size 8MB \
    0x2000 "$BOOTLOADER_BIN" \
    0x20000 "$PARTITION_BIN" \
    0x30000 "$LOADER_BIN" \
    0x140000 "$RAW_PAYLOAD_BIN"

log_success "Flash complete."

# ------------------------------------------------------------------------------
# Step 5: Serial capture (timeout 30s)
# ------------------------------------------------------------------------------
log_step "Step 5: Serial capture"
( cd "$BOOTLOADER_DIR" && idf.py -p "$PORT" monitor )

exit 0
