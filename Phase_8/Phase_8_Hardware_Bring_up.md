# Phase 8: Hardware Bring-up & Real Silicon Validation

- **Date:** July 19, 2026
- **Author:** Mayank (wolgwang)
- **Project:** SeedSigner Secure Boot — Summer of Bitcoin

## 1. Overview
In Phase 8, I transition from QEMU/Wokwi emulation to real silicon testing. After receiving the physical hardware—specifically the Waveshare ESP32-S3-WROOM-1-N8R8 development board—I focus on running initial hardware sanity checks and porting the Phase 6 secure bootloader prototype to the physical device.

## 2. Initial Hardware Sanity Checks
Before deploying the complex bootloader, I flashed baseline examples to validate the USB/UART connection and the ESP-IDF toolchain.
- **`hello_world_esp32s3`**: Successfully compiled and flashed. Validated the Python environment, `idf.py` workflow, and serial monitor over `/dev/ttyACM*`. (Permissions required `sudo chmod a+rw`).
- **`blink_esp32s3`**: Successfully ran the RGB LED blink program. 
  - *Hardware Quirk Discovered:* The default ESP32-S3 RGB LED pins (48 and 21) did not work. On this specific Waveshare board, the WS2812 addressable RGB LED is routed to **GPIO 38**.

## 3. Bootloader Silicon Preparation (`hardware_test`)
I copied the functional proto-bootloader from Phase 6. To adapt it for physical hardware and ensure device safety, I made several critical modifications to revert Wokwi-specific emulation hacks:

1. **SD Card Boot Re-enabled**: Disabled `USE_SPI_FLASH_FOR_QEMU_TEST` so the bootloader relies on the physical SDMMC driver to mount a FAT32 SD card rather than pulling payload data directly from raw flash memory.
2. **Partition Table Injection Removed**: Deleted the runtime flash-overwriting hack used to inject partition tables for Wokwi. The real hardware will rely on standard ESP-IDF partition flashing.
3. **MMU Quirks Reverted**: Removed the loop that zeroed the entire MMU table. The code now respects the physical ESP32-S3 `SOC_MMU_VALID` architecture.
4. **Strict Anti-Brick Measures**: Aggressively stripped all `CONFIG_SECURE_BOOT` variables from `sdkconfig.defaults` to guarantee that the irreversible eFuses are NOT touched during early testing.

## 4. Prepared Testing Objectives (Deferred)
With the `hardware_test` bootloader prepared and building, I had queued the following validation run on the physical S3 board. These steps were **not** executed on silicon before S3 work was paused (see Section 5):

- Package the compiled `hello_world.bin` payload using `tools/generate_signed_payload.py` to create a valid, Specter-signed dummy `seed.bin`.
- Format a physical Micro SD card to FAT32 and copy the dummy `seed.bin` to the root directory.
- Flash the `hardware_test` bootloader to the board.
- Verify via the serial monitor that the bootloader mounts the SD card, validates the multisig signature, and jumps execution to the payload.

## 5. Blocker: The S3 Board Awaits Soldering
Shortly after preparing the `hardware_test` bootloader, S3 bring-up stalled. Handling the ESP32-S3 board without proper soldering proved difficult — the power/flash/serial connections were too fragile to trust for bootloader testing, and the risk of damaging the only S3 board outweighed the benefit of proceeding. I paused ESP32-S3 work and will resume once the board is soldered.

## 6. Outcome & Honest Reflection
- **What was built:** the ESP-IDF toolchain and serial workflow validated on real silicon (`hello_world_esp32s3`); the RGB LED blink driven from the correct Waveshare GPIO (`blink_esp32s3`, RGB LED on **GPIO 38**, not the default pins 48/21); and the Phase 6 proto-bootloader adapted into `hardware_test` with the four Wokwi-era reverts listed in Section 3.
- **Where it fell short:** the core Phase 8 deliverable — SD-card boot, Specter multisig validation, and JMP into the payload on real ESP32-S3 silicon — could not be validated. The `hardware_test` loader is prepared and buildable, but its physical run is pending the soldering fix.
- **The pivot:** rather than block on board handling, I pivoted to the newly arrived ESP32-P4 module (Waveshare ESP32-P4 WiFi6 Touch LCD 4.3 board), whose bring-up became Phase 9 and eventually the primary platform for Phases 9–13.

## 7. What Comes Next — Phase 9
Phase 9 begins the ESP32-P4 bring-up: I set up a fresh ESP-IDF v5.5 workspace for the P4, encounter the fundamental `0x40000000` flash-cache limitation, and split the strategy into a stateful flash-resident loader (Path 1) vs. strictly stateless PSRAM execution (Path 2). See `Phase_9/Phase_9_ESP32P4_Bring_up.md` for the full story.
