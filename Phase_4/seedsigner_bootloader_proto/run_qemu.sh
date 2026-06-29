#!/bin/bash
/home/wolgwang/.espressif/tools/qemu-xtensa/esp_develop_9.0.0_20240606/qemu/bin/qemu-system-xtensa -nographic -machine esp32s3 -m 4M \
    -drive file=merged_flash.bin,if=mtd,format=raw \
    -drive file=qemu_efuse.bin,if=none,format=raw,id=efuse \
    -global driver=nvram.esp32.efuse,property=drive,value=efuse
