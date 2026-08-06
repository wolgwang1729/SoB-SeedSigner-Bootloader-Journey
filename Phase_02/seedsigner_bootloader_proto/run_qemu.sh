#!/bin/bash
$(which qemu-system-xtensa) -nographic -machine esp32s3 -m 2M \
    -drive file=merged_flash.bin,if=mtd,format=raw
