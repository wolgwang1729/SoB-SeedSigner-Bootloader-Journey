#!/bin/bash
$(which qemu-system-xtensa) -nographic -machine esp32s3 -m 2M \
    -drive file=merged_flash.bin,if=mtd,format=raw \
    -drive file=qemu_efuse.bin,if=none,format=raw,id=efuse \
    -global driver=nvram.esp32.efuse,property=drive,value=efuse
