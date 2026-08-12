#!/usr/bin/env bash
# ==============================================================================
# build_wrong_key_loader.sh — Phase 14, Layer-1 attack T1/T3
#
# Clones the Phase_13 loader, generates a NEW RSA-3072 signing key (the eFuse
# digest was burned with the ORIGINAL key), and builds the loader + bootloader
# signed with the wrong key. Flashing these on the board must be REJECTED by
# Secure Boot v2 before any code runs.
#
# SAFETY: the clone keeps CONFIG_EFUSE_VIRTUAL=y, so no physical eFuse is ever
# touched. The clone's build dir is gitignored. Use restore_good.sh afterwards.
#
# Output:
#   wrong_key_loader/build/bootloader/bootloader.bin        (wrong key)
#   wrong_key_loader/build/seedsigner_secure_loader.bin     (wrong key)
# ==============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHASE13_LOADER="$(cd "$SCRIPT_DIR/../../Phase_13/seedsigner_bootloader_p4_stateless_os" && pwd)"
CLONE="$SCRIPT_DIR/wrong_key_loader"

if [ ! -d "$PHASE13_LOADER" ]; then
    echo "[FAIL] Phase_13 loader not found at $PHASE13_LOADER"
    exit 1
fi

# --- source ESP-IDF ---
if ! command -v idf.py >/dev/null 2>&1; then
    for candidate in "$HOME/esp/esp-idf-v5.5/export.sh" "$HOME/esp/esp-idf/export.sh"; do
        [ -f "$candidate" ] && { . "$candidate" >/dev/null 2>&1; break; }
    done
fi
if ! command -v idf.py >/dev/null 2>&1; then
    echo "[FAIL] idf.py not found. Source ESP-IDF export.sh first."
    exit 1
fi

# --- clone + regenerate signing key ---
rm -rf "$CLONE"
cp -r "$PHASE13_LOADER" "$CLONE"
rm -rf "$CLONE/build" "$CLONE/sdkconfig"
rm -f "$CLONE/secure_boot_signing_key.pem"

echo "[INFO] Generating a NEW (wrong) RSA-3072 signing key..."
espsecure.py generate_signing_key --version 2 "$CLONE/wrong_signing_key.pem"
cp "$CLONE/wrong_signing_key.pem" "$CLONE/secure_boot_signing_key.pem"
grep -q "CONFIG_EFUSE_VIRTUAL=y" "$CLONE/sdkconfig.defaults" \
    && echo "[PASS] clone keeps CONFIG_EFUSE_VIRTUAL=y" \
    || { echo "[FAIL] virtual eFuse guard missing in clone!"; exit 1; }

echo "[INFO] Building loader signed with the WRONG key..."
( cd "$CLONE" && idf.py build ) || { echo "[FAIL] build failed"; exit 1; }

echo
echo "[PASS] Wrong-key artifacts ready:"
echo "  bootloader : $CLONE/build/bootloader/bootloader.bin"
echo "  loader     : $CLONE/build/seedsigner_secure_loader.bin"
echo
echo "To test: flash the WRONG-KEY loader at 0x30000 and expect SBv2 to reject:"
echo "  esptool.py --chip esp32p4 -p \${ESPPORT:-/dev/ttyACM0} -b 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 8MB 0x30000 $CLONE/build/seedsigner_secure_loader.bin"
echo "Then restore: $SCRIPT_DIR/restore_good.sh"
exit 0
