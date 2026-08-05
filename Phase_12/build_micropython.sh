#!/usr/bin/env bash
# ==============================================================================
# Sign MicroPython Payload Script — Phase 12
# Target  : ESP32-P4 Stateless Secure Loader
# Payload : MicroPython app image built in the external repo
#           ~/Desktop/seedsigner-micropython-builder (NOT built here — see
#           seedsigner_micropython_builder_changes/README.md). This script only
#           wraps the pre-built micropython.bin into a Specter-signed firmware
#           bundle for the SD card.
#           Output: Phase_12/build/seedsigner_esp32p4.bin  →  copy to a FAT32
#           SD card root (loader reads /sdcard/seedsigner_esp32p4.bin at boot).
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
# Paths
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR"
BOOTLOADER_DIR="$ROOT_DIR/seedsigner_bootloader_p4_stateless_os"
PAYLOAD_BIN="${MICROPYTHON_BIN:-$HOME/Desktop/seedsigner-micropython-builder/build/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/micropython.bin}"
SIGN_TOOL="$BOOTLOADER_DIR/tools/generate_signed_payload.py"
SIGNED_PAYLOAD_BIN="$ROOT_DIR/build/seedsigner_esp32p4.bin"

# ------------------------------------------------------------------------------
# Step 1: Check payload exists
# ------------------------------------------------------------------------------
log_step "Step 1: Locate MicroPython payload"
if [ ! -f "$PAYLOAD_BIN" ]; then
    log_error "Payload binary not found: $PAYLOAD_BIN"
    log_error "Run the external builder first (see seedsigner_micropython_builder_changes/README.md)."
    exit 1
fi
log_success "Payload binary: $PAYLOAD_BIN ($(wc -c < "$PAYLOAD_BIN") bytes)"

# ------------------------------------------------------------------------------
# Step 2: Sign payload (Specter secure app loader format)
# ------------------------------------------------------------------------------
log_step "Step 2: Sign payload"
if [ ! -f "$SIGN_TOOL" ]; then
    log_error "Signing tool not found: $SIGN_TOOL"
    exit 1
fi
mkdir -p "$ROOT_DIR/build"
python3 "$SIGN_TOOL" "$PAYLOAD_BIN" "$SIGNED_PAYLOAD_BIN" || {
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
