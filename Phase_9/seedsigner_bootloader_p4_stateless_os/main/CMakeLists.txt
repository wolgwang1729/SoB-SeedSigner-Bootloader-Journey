idf_component_register(SRCS "main.c" PRIV_REQUIRES specter_crypto fatfs esp_mm spi_flash nvs_flash mbedtls sdmmc)

# Note: For ESP32-P4 hardware testing, the signed payload (seed.bin) is loaded
# from a physical SD card, not from an embedded FAT partition in SPI flash.
# See the tools/ directory for generate_signed_payload.py to create seed.bin.
