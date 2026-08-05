#!/usr/bin/env bash
# ==============================================================================
# Build & Sign Payload Script — Phase 12
# Target  : ESP32-P4 Stateless Secure Loader
# Payload : Builds the stock hello-world payload (hello_world_esp32p4_stock_shim)
#           and wraps it into a Specter-signed firmware bundle for the SD card.
#           Output: Phase_12/build/seedsigner_esp32p4.bin  →  copy to a FAT32 SD
#           card root (loader reads /sdcard/seedsigner_esp32p4.bin at boot).
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
ROOT_DIR="$SCRIPT_DIR"
PAYLOAD_DIR="$ROOT_DIR/hello_world_esp32p4_stock_shim"
BOOTLOADER_DIR="$ROOT_DIR/seedsigner_bootloader_p4_stateless_os"

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
# Step 2: Sign payload (Specter secure app loader format)
# ------------------------------------------------------------------------------
log_step "Step 2: Sign payload"
SIGN_TOOL="$BOOTLOADER_DIR/tools/generate_signed_payload.py"
if [ ! -f "$SIGN_TOOL" ]; then
    log_error "Signing tool not found: $SIGN_TOOL"
    exit 1
fi
mkdir -p "$ROOT_DIR/build"
SIGNED_PAYLOAD_BIN="$ROOT_DIR/build/seedsigner_esp32p4.bin"
python3 "$SIGN_TOOL" "$RAW_PAYLOAD_BIN" "$SIGNED_PAYLOAD_BIN" || {
    log_error "Signing failed (need python3 ecdsa + bech32 packages)."
    exit 1
}
if [ ! -s "$SIGNED_PAYLOAD_BIN" ]; then
    log_error "Signed payload not produced."
    exit 1
fi
log_success "Signed payload: $SIGNED_PAYLOAD_BIN ($(wc -c < "$SIGNED_PAYLOAD_BIN") bytes)"
log_warn "Copy $SIGNED_PAYLOAD_BIN to a FAT32 SD card root as"
log_warn "  'seedsigner_esp32p4.bin'  →  insert the SD card, then reset the board."

exit 0
