#!/usr/bin/env bash
# ==============================================================================
# Build & Sign Test Shim Payload Script — Phase 17
# Target  : ESP32-S3 Stateless Secure Loader
# Payload : Builds hello_world_esp32s3_stock_shim and wraps into Specter bundle.
# Output  : Phase_17/build/seedsigner_esp32s3.bin
# ==============================================================================
set -euo pipefail

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
PAYLOAD_DIR="$ROOT_DIR/hello_world_esp32s3_stock_shim"
BOOTLOADER_DIR="$ROOT_DIR/seedsigner_bootloader_esp32s3_stateless_os"
SIGN_TOOL="$BOOTLOADER_DIR/tools/generate_signed_payload.py"
OUTPUT_DIR="$ROOT_DIR/build"
SIGNED_PAYLOAD_BIN="$OUTPUT_DIR/seedsigner_esp32s3.bin"

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

mkdir -p "$OUTPUT_DIR"

# ------------------------------------------------------------------------------
# Step 1: Build payload
# ------------------------------------------------------------------------------
log_step "Step 1: Build payload"
if [ ! -d "$PAYLOAD_DIR" ]; then
    log_error "Payload directory not found: $PAYLOAD_DIR"
    exit 1
fi
( cd "$PAYLOAD_DIR" && idf.py set-target esp32s3 && idf.py build )

RAW_PAYLOAD_BIN=$(find "$PAYLOAD_DIR/build" -maxdepth 1 -name "*.bin" \
    ! -name "bootloader.bin" ! -name "partition-table.bin" | head -1)
if [ -z "$RAW_PAYLOAD_BIN" ] || [ ! -s "$RAW_PAYLOAD_BIN" ]; then
    log_error "Payload binary not found under $PAYLOAD_DIR/build"
    exit 1
fi
log_success "Built payload binary: $RAW_PAYLOAD_BIN ($(wc -c < "$RAW_PAYLOAD_BIN") bytes)"

# ------------------------------------------------------------------------------
# Step 2: Sign payload (Specter secure app loader format for ESP32-S3)
# ------------------------------------------------------------------------------
log_step "Step 2: Sign payload"
if [ ! -f "$SIGN_TOOL" ]; then
    log_error "Signing tool not found: $SIGN_TOOL"
    exit 1
fi

PLATFORM_ATTR="seedsigner_esp32s3" python3 "$SIGN_TOOL" "$RAW_PAYLOAD_BIN" "$SIGNED_PAYLOAD_BIN" || {
    log_error "Signing failed (need python3 ecdsa + bech32 packages)."
    exit 1
}

if [ ! -s "$SIGNED_PAYLOAD_BIN" ]; then
    log_error "Signed payload not produced."
    exit 1
fi

log_success "Signed payload bundle: $SIGNED_PAYLOAD_BIN ($(wc -c < "$SIGNED_PAYLOAD_BIN") bytes)"
log_warn "Copy $SIGNED_PAYLOAD_BIN to a FAT32 SD card root as"
log_warn "  'seedsigner_esp32s3.bin'  -> insert SD card, then boot loader."

exit 0
