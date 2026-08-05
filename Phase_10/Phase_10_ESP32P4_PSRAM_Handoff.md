# Phase 10: ESP32-P4 PSRAM Handoff — Simplified Bootloader

- **Date:** July 27, 2026
- **Author:** Mayank (wolgwang)
- **Project:** SeedSigner Secure Boot — Summer of Bitcoin

## 1. Hardware: Waveshare ESP32-P4-Module-DEV-KIT

> **Board purchased for Phase 10 development and validation.**

**Waveshare ESP32-P4-Module High-performance Development Board** — Based on ESP32-P4 and ESP32-C6, supports Wi-Fi 6 and Bluetooth 5/BLE.

### 1.1 Description

ESP32-P4-Module-DEV-KIT is a dual-core RISC-V high-performance development board based on the ESP32-P4 chip, designed by Waveshare. It supports a wide range of human-computer interfaces, including MIPI-CSI (with integrated Image Signal Processor, ISP) and MIPI-DSI interfaces, as well as common peripherals such as SPI, I2S, I2C, LED PWM, MCPWM, RMT, ADC, UART, and TWAI™. It also supports USB OTG 2.0 HS, Ethernet, and SDIO Host 3.0 for high-speed connectivity. The chip integrates a digital signature peripheral and a dedicated key management unit to ensure security.

### 1.2 Key Features

- **MCU:** RISC-V 32-bit dual-core (HP, up to 400 MHz) + single-core (LP, up to 40 MHz)
- **Memory:** 128 KB HP ROM · 16 KB LP ROM · 768 KB HP L2MEM · 32 KB LP SRAM · 8 KB TCM
- **PSRAM:** 32 MB stacked in-package
- **Flash (on-module):** 16 MB NOR Flash  
  > **Project constraint:** firmware targets **8 MB flash** so the bootloader and payload are directly portable to any ESP32-P4 variant shipped with 8 MB flash, without modification.
- **Wi-Fi / BT co-processor:** ESP32-C6 (via SDIO) — Wi-Fi 6 & Bluetooth 5/BLE
- **Image & video:** JPEG Codec · Pixel Processing Accelerator (PPA) · ISP · H.264 encoder (1080p @ 30 fps)
- **Security:** Secure Boot · Flash Encryption · Cryptographic accelerators · TRNG · Hardware access protection / privilege separation

### 1.3 Peripheral Interfaces

| Interface | Detail |
|---|---|
| GPIO headers | 2×20 pin, 28 remaining programmable GPIOs |
| USB | Type-A USB 2.0 OTG (HOST/DEVICE) |
| Ethernet | 100 Mbps with reserved PoE module header |
| Storage | SDIO 3.0 TF card slot |
| Programming | Type-C UART flashing port |
| Camera | MIPI-CSI (full HD 1080p acquisition) |
| Display | MIPI-DSI + 2D DMA (1080p @ 30 fps JPEG decode) |
| Audio | Speaker interface · Microphone · 3.5 mm headphone jack |
| RTC | RTC battery header |
| I2C / I2S / SPI / ADC | Standard peripheral headers |

---

## 2. Overview

Take the Phase 9 stateless bootloader (`seedsigner_bootloader_p4_stateless_os`), strip out the Specter secp256k1 signature verification to reduce complexity, and focus purely on getting a PSRAM-resident payload to execute correctly on real ESP32-P4 silicon.

Once the bare MMU handoff is proven to work end-to-end, signature verification can be layered back in.

## 3. Target

1. Copy `seedsigner_bootloader_p4_stateless_os` from Phase 9 as the starting point.
2. Remove all Specter-DIY verification logic (`blsig`, `blsect`, `secp256k1`) — the bootloader simply reads the raw firmware binary from the SD card, no signature check.
3. Load the binary into PSRAM, remap the MMU, and jump.
4. Validate that the payload executes correctly on real P4 silicon.
5. Once the PSRAM jump is stable, re-introduce signature verification on top.

## 4. Staged Debugging Plan (if jump still fails)

If the simplified bootloader still fails to hand off to PSRAM, go brick by brick:

1. **Stage 0 — Minimal Jump:** Flash the bootloader. Have it do nothing except disable watchdogs and jump to a fixed known address in internal SRAM (HP L2MEM, `0x4FF00000`). Pre-fill that address with a small hand-coded RISC-V stub that toggles a GPIO. If the GPIO toggles, the bare jump works.

2. **Stage 1 — SRAM Payload:** Build a minimal payload that copies itself into `0x4FF00000` and runs from there. No PSRAM, no MMU remapping. Just verify that the bootloader can hand control to arbitrary code in internal RAM.

3. **Stage 2 — PSRAM Read:** Verify that the bootloader can read from PSRAM correctly after the cache and MMU are in the post-jump state. Establish exactly what cache operations are needed and in what order.

4. **Stage 3 — Single-Page PSRAM Map:** Map exactly one 64KB page of PSRAM to one virtual address. Write a known pattern. Read it back from the virtual address. Only once this works reliably does any code run from it.

5. **Stage 4 — Full Handoff:** Extend the single-page test to a real payload.

## 5. Better Payload

Instead of the bare-metal `hello_world_esp32p4_stateless_payload` from Phase 9 (which only printed over UART), try a richer payload to exercise more of the execution environment:

- A payload that initializes the SDMMC peripheral and reads back a file from the SD card (proves peripheral access from PSRAM context works).
- A payload that exercises PSRAM reads and writes in a loop (stress-tests cache coherency after the MMU remap).
- MicroPython as the ultimate payload — if MicroPython boots and the REPL responds, the handoff is production-grade.

## 6. Execution & Tests (The Brick-by-Brick Approach)

This section details the "brick by brick" testing strategy implemented for the `seedsigner_bootloader_p4_stateless_os`. Since the baseline PSRAM handoff (Brick 1) is already proven, the focus was on progressively validating cache edge cases, peripheral access, and finally injecting FreeRTOS back into the environment safely.

### Brick 1: Baseline Handoff (Completed)
- **Goal:** Prove the custom bootloader can extract a payload, map it into PSRAM, flush caches, disable watchdogs/interrupts, and execute a jump.
- **Status:** **PASS.** The bare-metal payload prints `PHASE 10: HELLO FROM BARE METAL PAYLOAD!` successfully over UART.

### Brick 2: PSRAM Cache Coherency & Edge Cases (Completed)
Just because code executes from PSRAM doesn't mean the data cache is fully coherent across 64KB page boundaries after the MMU remap.
- **Test Objective:** Write and read massive arrays in PSRAM.
- **Implementation:** 
  - Manually allocated an additional 1MB of PSRAM memory mappings by directly writing to the MMU `INDEX` and `CONTENT` registers (`0x48800000` -> `0x01000000`) from the payload.
  - Wrote a pseudo-random `0xA5A5....` pattern across the entire 1MB space.
  - Invalidated the D-cache manually by thrashing a 64KB scratch buffer to force the hardware cache controller to flush my writes out to physical PSRAM.
  - Read back the array and successfully verified all values.
- **Status:** **PASS.** The 1MB array verified perfectly, proving I can manually manipulate the MMU from bare-metal supervisor mode and that my cache strategy is 100% coherent.

### Brick 3: Bare-Metal Peripheral Access (Permissions Check) (Completed)
Before bringing in the complexity of ESP-IDF drivers, I must prove the payload has the necessary hardware access privileges (PMP/SMP registers) after the bootloader hands off.
- **Test Objective:** Read directly from memory-mapped hardware peripheral registers.
- **Implementation:**
  - Avoided any GPIO/LED or eFuse writes to ensure the board remains pristine.
  - Read the RISC-V internal CSR CPU cycle counter.
  - Performed direct memory-mapped reads of the `SYSTIMER` hardware configuration and interrupt status registers located in the `0x50100000` hardware peripheral space.
- **Status:** **PASS.** The payload successfully read the `SYSTIMER` peripheral registers in a loop without triggering a PMP/SMP privilege violation or Load Access Fault.

### Brick 4: Minimal FreeRTOS Handoff (The "Rug Pull" Test) (Completed)
This was the most critical hurdle. Standard ESP-IDF `call_start_cpu0` expects to be the master bootloader and will attempt to reset the MMU, hardware peripherals, and flash cache, which would instantly crash my stateless PSRAM environment.
- **Test Objective:** Run the FreeRTOS scheduler inside the stateless payload without crashing the MMU or triggering watchdogs.
- **Implementation & Challenges:**
  1. **Intercepting Hardware Resets:** I used GNU Linker `--wrap` flags in CMake to intercept critical ESP-IDF startup functions like `Cache_Resume_DCache`, `spi_flash_init`, `esp_reset_reason_init`, and `wdt_hal_init`. This prevented `call_start_cpu0` from re-configuring the MMU and resetting my external PSRAM mappings.
  2. **The "Silent Hang" inside SRAM:** Even with hardware resets bypassed, the CPU silently hung upon jumping into `call_start_cpu0` (which resides in `.iram1.text` inside the Internal SRAM at `0x4FF00000`).
  3. **I-Cache Coherency Bug:** I discovered the CPU was fetching stale instructions (left over from the ROM bootloader) from the I-Cache. I initially tried `fence.i`, but it only flushes the CPU's local pipeline, not the hardware L1 Cache controller. I also tried `Cache_Invalidate_Addr`, but that caused an exception because it's only meant for External memory (`0x4800....`).
  4. **The Fix:** I successfully invoked the internal ROM function `Cache_Invalidate_All(0x03)` to forcefully invalidate `CACHE_L1_ICACHE0` and `CACHE_L1_ICACHE1`, followed by `fence.i`. This flushed the SRAM I-Cache, allowing the CPU to execute the freshly copied `call_start_cpu0` instructions.
  5. **Result:** The `app_main` executed, spawning a FreeRTOS task that dynamically allocated memory and printed "Hello from a FreeRTOS Task running completely in PSRAM!" in a loop.
- **Status:** **PASS.** The payload booted fully into FreeRTOS, dynamically allocating memory from the PSRAM heaps and executing the scheduler flawlessly out of external memory.

### Brick 5: The "Rich" Payload — Standard C Library & SDMMC (Completed)
Once FreeRTOS is ticking properly in PSRAM, the environment is identical to a standard ESP-IDF app, but fully stateless.
- **Test Objective:** Initialize high-level ESP-IDF drivers and prove peripheral access from the PSRAM context.
- **Implementation & Challenges:**
  1. **Standard C Library (`printf`):** Verified that the Newlib/VFS layer works correctly. Both `esp_rom_printf` (ROM) and standard `printf` (VFS/UART) produce output, confirming the Virtual File System is fully initialized.
  2. **SDMMC Power Control:** The ESP32-P4 requires an on-chip LDO (Low Dropout Regulator) on channel 4 to power the SDMMC IO lines. I used the official `sd_pwr_ctrl_new_on_chip_ldo` API to enable it — simple GPIO toggling was insufficient.
  3. **Pin Configuration:** Configured the Waveshare ESP32-P4-Module-DEV-KIT SDMMC Slot 1 pins via GPIO matrix: `CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42`.
  4. **FAT Filesystem Mount:** Successfully mounted a physical 16GB SDHC card (FAT32) at 20 MHz in 4-bit bus mode using `esp_vfs_fat_sdmmc_mount`.
  5. **File I/O:** Read a user-created `hello.txt` file from the SD card using standard POSIX `fopen`/`fgets`, printing `Read from file: 'Hello From SD Card'`.
  6. **Clean Unmount:** The SD card was unmounted cleanly via `esp_vfs_fat_sdcard_unmount` and the LDO driver was released.
  7. **Running Pristine ESP-IDF Code:** Successfully built and executed the unmodified stock ESP-IDF `hello_world` app as the payload by moving all my `__wrap` intercepts into a standalone `stateless_shim` component. The stock app printed chip information and FreeRTOS heap size just like it would normally, completely oblivious that it was running under the PSRAM bootloader handoff.
- **Status:** **PASS.** The stateless PSRAM payload successfully powered the SD card via on-chip LDO, mounted a FAT32 filesystem, read a file using standard POSIX I/O, and unmounted cleanly. I also ran a stock ESP-IDF application from PSRAM successfully. This proves the bootloader handoff supports full peripheral and driver access.

### Brick 6: MicroPython as Ultimate Payload (Stretch Goal)
- **Test Objective:** Compile MicroPython for ESP32-P4 and boot the REPL from the stateless PSRAM environment.
- **Implementation & Challenges:**
  1. **Compilation & Shim:** Built MicroPython for `WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43` using the SeedSigner builder. Integrated the `stateless_shim` to intercept hardware resets (`cache_hal_init`, `esp_mmu_map_init`, `esp_psram_init`, etc.) that would otherwise break the PSRAM MMU context.
  2. **Firmware Size:** Increased the bootloader's `MAX_FIRMWARE_SIZE` to 8 MB to accommodate the massive MicroPython binary (~4.4 MB).
  3. **The `.bss` Overwrite Hazard:** I discovered MicroPython's enormous `.bss` segment spans all the way up to `0x4FF6118C`. This would completely overwrite the default bootloader stack during `call_start_cpu0`'s `init_bss` step.
  4. **Stack Relocation & L2 Cache Trap:** I attempted to relocate the stack pointer (`sp`) in `my_entry_point` to `0x4FFBFFF0` to keep it safe. However, the ESP32-P4 ROM bootloader configures the top 128KB of HP SRAM (from `0x4FFA0000` to `0x4FFC0000`) as L2 Cache. By placing the stack inside the L2 Cache memory space, I inadvertently corrupted the cache tags. When the CPU tried to fetch instructions from PSRAM, it hit the corrupted cache lines, fetching garbage and crashing. I resolved this by relocating the stack to `0x4FF9FFF0` (safely below the L2 Cache and above MicroPython's `.bss`).
- **Final Result:** **STALLED**. Despite fixing the stack and memory layout issues, the CPU still hangs silently immediately upon entering `call_start_cpu0`. The exact cause is obscured because standard UART debugging output fails to flush (likely due to double faults, watchdog resets during the massive `.bss` zeroing, or deeper architectural state assumptions violated by MicroPython compared to a pristine ESP-IDF `hello_world`). As this is a stretch goal, the attempt was concluded here.
