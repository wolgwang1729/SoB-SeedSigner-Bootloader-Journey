#!/usr/bin/env bash
# ==============================================================================
# restore_good.sh — Phase 14: restore the board to the GOOD Phase_13 artifacts
# after a Layer-1 attack test. Refuses to run unless the clone still has
# CONFIG_EFUSE_VIRTUAL=y (safety guard). Reuses Phase_13 run.sh build output.
# ==============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHASE13_LOADER="$(cd "$SCRIPT_DIR/../../Phase_13/seedsigner_bootloader_p4_stateless_os" && pwd)"
PORT="${ESPPORT:-/dev/ttyACM0}"
BAUD="${ESPBAUD:-921600}"

if ! command -v esptool.py >/dev/null 2>&1; then
    for candidate in "$HOME/esp/esp-idf-v5.5/export.sh" "$HOME/esp/esp-idf/export.sh"; do
        [ -f "$candidate" ] && { . "$candidate" >/dev/null 2>&1; break; }
    done
fi
command -v esptool.py >/dev/null 2>&1 || { echo "[FAIL] esptool.py not found"; exit 1; }

# safety: never restore over a physical-eFuse setup we can't see
grep -q "CONFIG_EFUSE_VIRTUAL=y" "$PHASE13_LOADER/sdkconfig.defaults" \
    || { echo "[FAIL] Phase_13 loader does NOT use virtual eFuses — aborting."; exit 1; }
grep -q "CONFIG_EFUSE_VIRTUAL=y" "$PHASE13_LOADER/sdkconfig" 2>/dev/null \
    || { echo "[FAIL] Phase_13 sdkconfig missing CONFIG_EFUSE_VIRTUAL=y — build run.sh first."; exit 1; }

if [ ! -e "$PORT" ]; then
    echo "[FAIL] port $PORT not found (board attached?)"
    exit 1
fi

BOOTLOADER="$PHASE13_LOADER/build/bootloader/bootloader.bin"
PARTITION="$PHASE13_LOADER/build/partition_table/partition-table.bin"
LOADER="$PHASE13_LOADER/build/seedsigner_secure_loader.bin"
for f in "$BOOTLOADER" "$PARTITION" "$LOADER"; do
    [ -s "$f" ] || { echo "[FAIL] missing $f — run Phase_13 run.sh build first"; exit 1; }
done

echo "[INFO] Restoring GOOD bootloader (0x2000), partition table (0x20000), loader (0x30000)..."
esptool.py --chip esp32p4 --port "$PORT" --baud "$BAUD" \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_freq 40m --flash_size 8MB \
    0x2000 "$BOOTLOADER" \
    0x20000 "$PARTITION" \
    0x30000 "$LOADER"
echo "[PASS] Good artifacts restored."
exit 0
