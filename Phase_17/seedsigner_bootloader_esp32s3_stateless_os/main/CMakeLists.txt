idf_component_register(
    SRCS "main.c" "anti_phish.c" "bip39_wordlist.c"
    PRIV_REQUIRES esp_mm spi_flash esp_partition fatfs sdmmc esp_driver_sdspi esp_driver_spi specter_crypto bootloader_support
)

# Phase 15 (ESP32-S3): move the loader's "point of no return" trampoline into a
# dedicated high-IRAM JMP zone (0x403A0000-0x403B8000), clear of the payload's
# IRAM copy targets (0x40374000+) and of the 2nd-stage bootloader's window
# (0x403CB700). This script is a MEMORY redefinition + appended SECTIONS that
# must land after esp_system's memory.ld/sections.ld on the link line — linked
# after esp_system, so it does.
target_linker_script(${COMPONENT_LIB} INTERFACE "${CMAKE_CURRENT_LIST_DIR}/ld/loader_high.ld")

# Phase 12: The payload is a Specter-signed firmware bundle read from the SD
# card (/sdcard/seedsigner_esp32s3.bin). The loader verifies the secp256k1
# multisig signature against the vendor keys in main.c before parsing/executing
# the image. The SD card is unmounted immediately after the read (TOCTOU-safe),
# then the loader executes the verified image entirely from PSRAM.
