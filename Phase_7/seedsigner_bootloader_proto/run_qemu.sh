#!/bin/bash
# QEMU path is machine-specific; override with QEMU=/path/to/qemu-system-xtensa
QEMU="${QEMU:-$(command -v qemu-system-xtensa)}"
if [ -z "$QEMU" ] && [ -f "$HOME/.espressif/tools/qemu-xtensa/esp_develop_9.2.2_20250817/qemu/bin/qemu-system-xtensa" ]; then
    QEMU="$HOME/.espressif/tools/qemu-xtensa/esp_develop_9.2.2_20250817/qemu/bin/qemu-system-xtensa"
fi
if [ -z "$QEMU" ]; then
    echo "ERROR: qemu-system-xtensa not found. Set QEMU=/path/to/qemu-system-xtensa" >&2
    exit 1
fi
"$QEMU" -nographic -machine esp32s3 -m 4M \
    -drive file=merged_flash.bin,if=mtd,format=raw \
    -drive file=qemu_efuse.bin,if=none,format=raw,id=efuse \
    -global driver=nvram.esp32.efuse,property=drive,value=efuse
