idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES esp_mm spi_flash esp_partition
)

# Phase 10: Payload (seed.bin) is a raw ESP32 image flashed into the
# 'payload' partition (0x140000, ~12 MB). The loader reads it directly
# from flash into PSRAM at boot — no SD card or embedded blob required.
# Flash the payload separately:
#   esptool.py --port /dev/ttyUSB0 write_flash 0x140000 seed.bin
