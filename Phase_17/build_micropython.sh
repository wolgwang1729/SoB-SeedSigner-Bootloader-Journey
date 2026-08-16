#!/usr/bin/env bash
# ==============================================================================
# Build & Sign S3 MicroPython Payload for SD Card — Phase 17
# Target  : ESP32-S3 Stateless Secure Loader (Pure SD-Card Boot)
# Payload : MicroPython app image built in the external repo
#           ~/Desktop/seedsigner-micropython-builder (branch: esp32s3-stateless-boot).
#           This script wraps the pre-built micropython.bin into a Specter-signed
#           firmware bundle for the SD card.
# Output  : Phase_17/build/seedsigner_esp32s3.bin  (Specter bundle for SD card)
#           -> Copy to FAT32 SD card root as /seedsigner_esp32s3.bin
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
# Paths
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR"
BOOTLOADER_DIR="$ROOT_DIR/seedsigner_bootloader_esp32s3_stateless_os"
PAYLOAD_BIN="${MICROPYTHON_BIN:-$HOME/Desktop/seedsigner-micropython-builder/build/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/micropython.bin}"
SIGN_TOOL="$BOOTLOADER_DIR/tools/generate_signed_payload.py"
OUTPUT_DIR="$ROOT_DIR/build"
SIGNED_PAYLOAD_BIN="$OUTPUT_DIR/seedsigner_esp32s3.bin"

mkdir -p "$OUTPUT_DIR"

# ------------------------------------------------------------------------------
# Step 1: Check payload exists
# ------------------------------------------------------------------------------
log_step "Step 1: Locate MicroPython payload"
if [ ! -f "$PAYLOAD_BIN" ]; then
    log_error "Payload binary not found: $PAYLOAD_BIN"
    log_error "Run the external builder first:"
    log_error "  cd ~/Desktop/seedsigner-micropython-builder"
    log_error "  make docker-build-all BOARD=WAVESHARE_ESP32_S3_TOUCH_LCD_35B"
    exit 1
fi
log_success "Found MicroPython binary: $PAYLOAD_BIN ($(wc -c < "$PAYLOAD_BIN") bytes)"

# ------------------------------------------------------------------------------
# Step 2: Sign payload (Specter secure app loader format for ESP32-S3)
# ------------------------------------------------------------------------------
log_step "Step 2: Sign payload (Specter format)"
if [ ! -f "$SIGN_TOOL" ]; then
    log_error "Signing tool not found: $SIGN_TOOL"
    exit 1
fi

PLATFORM_ATTR="seedsigner_esp32s3" python3 "$SIGN_TOOL" "$PAYLOAD_BIN" "$SIGNED_PAYLOAD_BIN" || {
    log_error "Signing failed (need python3 ecdsa + bech32 packages)."
    exit 1
}

if [ ! -s "$SIGNED_PAYLOAD_BIN" ]; then
    log_error "Signed payload not produced."
    exit 1
fi

log_success "Signed payload bundle: $SIGNED_PAYLOAD_BIN ($(wc -c < "$SIGNED_PAYLOAD_BIN") bytes)"
log_warn "Copy $SIGNED_PAYLOAD_BIN to a FAT32 SD card root as:"
log_warn "  'seedsigner_esp32s3.bin'  -> insert SD card into ESP32-S3 board, then power on."

exit 0
