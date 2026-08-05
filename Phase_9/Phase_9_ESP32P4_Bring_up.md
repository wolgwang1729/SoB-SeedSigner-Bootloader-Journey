# Phase 9: ESP32-P4 Bring-up

- **Date:** July 24, 2026
- **Author:** Mayank (wolgwang)
- **Project:** SeedSigner Secure Boot — Summer of Bitcoin

## 1. Overview
Due to difficulties handling the ESP32-S3 board without proper soldering, work on the ESP32-S3 has been temporarily paused. I am creating this new phase to pivot my focus to the ESP32-P4 board and begin its bring-up process.

## 2. Architectural Limitation: The Two Paths

During the ESP32-P4 bring-up, I immediately encountered a fundamental hardware constraint that forced a fork in strategy:

- **The Problem:** On the ESP32-P4, the Cache Controller hard-routes the instruction cache virtual address range (`0x40000000`) exclusively to SPI0 (Flash). It is physically impossible to map `0x40000000` to PSRAM directly.
- **The Consequence:** Standard ESP-IDF applications are linked to execute from `0x40000000`. This means a payload built with default linker settings *cannot* run from PSRAM — the cache hardware simply won't serve instructions from there.

This forced a decision between two architecturally different approaches:

### Path 1: Stateful OS, Stateless Secrets
Following discussions with Keith, the primary threat model concerns **user secrets (seeds/keys)** persisting after power-down, not necessarily the open-source OS itself. To satisfy this requirement on the ESP32-P4:
1. **Write the OS to Flash:** The bootloader writes the verified firmware from the SD card to the SPI flash partition.
2. **Fill the Void:** Immediately after writing the firmware, the bootloader uses the hardware TRNG to **completely fill the rest of the available SPI flash with random garbage**.
3. **Guarantee of Stateless Secrets:** Since the flash is 100% full, the MicroPython OS (even if compromised) literally has no empty sectors left to write a user's seed phrase to.
4. **Anti-Phishing Proof:** The random garbage can be hashed and displayed as an anti-phishing checksum on boot.

### Path 2: Strictly Stateless (The Deep Engineering Fix)
If strict stateless execution (no OS writes to flash) is non-negotiable, I must bypass the `0x40000000` flash-routing limitation entirely:
1. **Linker Script Modification:** Modify the payload's linker script so `.text` and `.rodata` sections are linked to the PSRAM address space (`0x48000000`) instead of the standard flash space (`0x40000000`).
2. **Direct PSRAM Execution:** The bootloader loads the SD card image directly into PSRAM, remaps the MMU, and jumps to `0x48000000` — executing purely from RAM without ever writing to SPI flash.

---

## 3. Folder Structure

This phase produced **four folders**, organized to cleanly separate the two paths and preserve the original baselines:

```
Phase_9/
├── hello_world_esp32p4/                    ← Stateful payload (Path 1)
│   └── A basic ESP-IDF "hello world" app
│       used to verify the board environment
│       (compile, flash, UART output). Built
│       with the standard linker layout that
│       executes from the 0x40000000 flash
│       cache window.
│
├── seedsigner_bootloader_p4_stateful_os/   ← Stateful bootloader (Path 1)
│   └── The bootloader for Path 1 (Stateful OS,
│       Stateless Secrets). Reads firmware from
│       SD card, writes it to the 'payload' flash
│       partition at 0x140000, then fills the
│       remaining flash with TRNG random data so
│       no secrets can be written post-boot.
│
├── hello_world_esp32p4_stateless_payload/  ← Stateless payload (Path 2)
│   └── A bare-metal hello world built for Path 2.
│       Linked to a custom entry point
│       (my_entry_point) and targeted at the
│       PSRAM address space (0x48000000). Used
│       as the dummy payload for testing the
│       stateless bootloader's MMU handoff.
│
└── seedsigner_bootloader_p4_stateless_os/  ← Stateless bootloader (Path 2)
    └── The bootloader for Path 2 (Strictly
        Stateless). Loads the signed SD card
        image entirely into PSRAM, performs
        secp256k1 multisig verification, and
        uses a bare-metal MMU hijack to remap
        0x48000000 → PSRAM physical address
        before jumping to the payload entry.
        No flash writes occur during the OS load.
```

---

## 4. Goals
1. Initialize a basic ESP32-P4 ESP-IDF project and verify the board environment.
2. Prototype **Path 1** (Stateful OS, Stateless Secrets) — write firmware to flash, fill remainder with random data.
3. Prototype **Path 2** (Strictly Stateless) — load firmware into PSRAM and execute via MMU remapping.
4. Validate P4-specific secure boot features (Secure Boot v2, chip revision quirks, SD card via SDMMC).

---

## 5. Status

- [x] Initialized Phase 9 documentation and documented the ESP32-P4 cache limitation.
- [x] Initialize `hello_world_esp32p4` basic project.
- [x] Build and flash `hello_world` on ESP32-P4 to verify environment and board.
- [x] Port bootloader prototype to ESP32-P4 (fix memory map, MMU, SD card peripheral).
- [x] Build `seedsigner_bootloader_p4_stateful_os` (Path 1 baseline).
- [x] Create `seedsigner_bootloader_p4_stateless_os` (Path 2 attempt).
- [x] Create `hello_world_esp32p4_stateless_payload` (bare-metal payload for Path 2 testing).
- [ ] Successfully execute a PSRAM-resident payload via MMU hijack on real P4 silicon.
- [ ] Implement random flash-fill mechanism (Path 1 completion).
- [ ] Validate full end-to-end SD card boot with signed payload (either path).

---

## 6. Testing: SD Card Boot on ESP32-P4

### Step 1: Build the Hello World as a Dummy Payload
```bash
cd ~/Desktop/SoB/Phase_9/hello_world_esp32p4
idf.py build
```

### Step 2: Generate the Signed Payload
Package the `hello_world.bin` into a signed `seed.bin` using the Specter signing tool:
```bash
cd ~/Desktop/SoB/Phase_9/seedsigner_bootloader_p4_stateful_os
python3 tools/generate_signed_payload.py \
    ../hello_world_esp32p4/build/hello_world.bin \
    seed.bin
```

### Step 3: Prepare the SD Card
1. Format a MicroSD card as **FAT32**.
2. Copy the generated `seed.bin` to the **root directory** of the SD card.
3. Safely eject the card and insert it into the ESP32-P4 board's SD card slot.

### Step 4: Build and Flash the Bootloader
```bash
cd ~/Desktop/SoB/Phase_9/seedsigner_bootloader_p4_stateful_os
idf.py fullclean
idf.py set-target esp32p4
idf.py build
# Verify virtual eFuses are enabled before flashing:
grep "CONFIG_EFUSE_VIRTUAL=y" build/sdkconfig
idf.py -p /dev/ttyACM0 flash monitor
```

### Expected Serial Output
```
I (xxx) SEEDSIGNER_LOADER: Starting SeedSigner Secure Loader (App Mode) — ESP32-P4
I (xxx) SEEDSIGNER_LOADER: Initializing SD card via SDMMC (native P4 interface)
I (xxx) SEEDSIGNER_LOADER: SD Card mounted successfully.
I (xxx) SEEDSIGNER_LOADER: Found firmware. Size: XXXXX bytes
I (xxx) SEEDSIGNER_LOADER: Unmounting storage to prevent TOCTOU attacks...
I (xxx) SEEDSIGNER_LOADER: Found Specter Bootloader header.
I (xxx) SEEDSIGNER_LOADER: Firmware version: 1
I (xxx) SEEDSIGNER_LOADER: Performing secp256k1 multisig verification...
I (xxx) SEEDSIGNER_LOADER: Signature verification PASSED!
I (xxx) SEEDSIGNER_LOADER: Image segments: X, Entry: 0xXXXXXXXX
I (xxx) SEEDSIGNER_LOADER: Loading segment 0: addr=0x..., len=...
...
I (xxx) SEEDSIGNER_LOADER: Jumping to entry point...
```

---

## 7. Troubleshooting & Fixes

### 1. Serial Port Permission Denied
**Issue:** Got `[Errno 13] Permission denied: '/dev/ttyACM0'` when trying to flash the board.
**Fix:** Added the user to the `dialout` group (`sudo usermod -a -G dialout $USER`) and refreshed the session, or used `sudo chmod a+rw /dev/ttyACM0` for a quick temporary fix.

### 2. ESP32-P4 Chip Revision Mismatch
**Issue:** Flashing failed with the error: `bootloader/bootloader.bin requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)`.
**Fix:** ESP-IDF v5.5 defaults to compiling for production silicon (v3+). Since my board has `v1.3` silicon, I updated the configuration via `idf.py menuconfig`:
- Navigated to: `Component config` -> `Chip revision`
- Enabled the option: `Select ESP32-P4 revisions <3.0 (No >=3.x Support)` (Kconfig variable: `ESP32P4_SELECTS_REV_LESS_V3`).
This compiled the bootloader for the older v1.3 silicon, allowing a successful flash.

### 3. SD Card Not Detected
**Issue:** SD card fails to mount with timeout error.
**Possible Fixes:**
- Ensure the SD card is formatted as **FAT32** (not exFAT or NTFS).
- The ESP32-P4 SD card slot power is controlled by an internal LDO. Check that the board's SD power rail is enabled.
- Enable internal pull-ups: `slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;` (already set in code).
- Try a different SD card — some cards have compatibility issues with 1-bit SDMMC mode.

---

## 8. Outcome & Honest Reflection

### What Was Built
The Path 2 stateless bootloader (`seedsigner_bootloader_p4_stateless_os`) was written in full. It implements:
- SD card read into PSRAM (`heap_caps_aligned_alloc(MALLOC_CAP_SPIRAM)`)
- secp256k1 multisig signature verification via the Specter-DIY pipeline
- TOCTOU-safe SD card unmounting before execution
- A bare-metal MMU hijack (`do_mmu_mapping_and_jump()`) that rewrites the SPI MMU table entries to point `0x48000000` → PSRAM physical address
- Full watchdog disable, interrupt disable, PMP clear, cache invalidation, and `fence.i` before the jump

### Where It Fell Short
Despite all of this, **the payload never successfully executed from PSRAM on real ESP32-P4 silicon.**

I underestimated the complexity of what was being attempted. On paper, the PSRAM execution path on ESP32-P4 is straightforward — map `0x48000000`, jump. In practice, it required navigating:
- The ESP32-P4's unified I/D cache architecture (RISC-V, not Xtensa — cache invalidation works differently)
- Pre-production v1.3 silicon quirks that aren't documented in the official TRM
- FreeRTOS context teardown sequencing conflicting with the cache state before the jump
- The MMU page table layout on P4 being subtly different from the S3 (SPI_MEM_C vs SPI_MEM_S registers)
- Cache coherency issues after remapping that caused instruction fetch faults at the entry point

Day after day, the jump either silently hung, hard-faulted, or the UART went dark at `JMP[13] JUMP!` — the final print before the entry call. The hardware gave no useful diagnostics after that point because the exception vector was also being remapped away.

### The Ray of Hope
After all those attempts, one thing *did* work: the **bare-metal stateless payload** (`hello_world_esp32p4_stateless_payload`) successfully printed its output over UART when flashed directly:

```
========================================
HELLO FROM BARE METAL PAYLOAD!!!
========================================
Still alive in payload...
Still alive in payload...
```

This is significant. It proved that a payload built without the standard ESP-IDF ROM startup chain — no `call_start_cpu0`, no IROM mapping, custom entry point only — *runs correctly on this silicon*. The payload side of the stateless architecture is sound. The problem is purely in the bootloader's handoff: getting the MMU remapped and the CPU into the payload's execution context cleanly.

---

## 9. What Comes Next — Phase 10

Phase 9 ends here. The goal was ambitious and the result is honest: partial progress, real hardware understanding, and one concrete proof-of-concept.

Phase 10 will be different in approach. Instead of trying to build the full MMU-hijack bootloader all at once, the plan is to go **brick by brick**:

1. **Stage 0 — Minimal Jump:** Flash the bootloader. Have it do nothing except disable watchdogs and jump to a fixed known address in internal SRAM (HP L2MEM, `0x4FF00000`). Pre-fill that address with a small hand-coded RISC-V stub that toggles a GPIO. If the GPIO toggles, the bare jump works.

2. **Stage 1 — SRAM Payload:** Build a minimal payload that copies itself into `0x4FF00000` and runs from there. No PSRAM, no MMU remapping. Just verify that the bootloader can hand control to arbitrary code in internal RAM.

3. **Stage 2 — PSRAM Read:** Verify that the bootloader can read from PSRAM correctly after the cache and MMU are in the post-jump state. Establish exactly what cache operations are needed and in what order.

4. **Stage 3 — Single-Page PSRAM Map:** Map exactly one 64KB page of PSRAM to one virtual address. Write a known pattern. Read it back from the virtual address. Only once this works reliably does any code run from it.

5. **Stage 4 — Full Handoff:** Extend the single-page test to a real payload. This is the stateless execution goal, reached incrementally rather than all at once.

The key lesson from Phase 9: this is a deep hardware problem, not a software logic problem. It requires treating the silicon as the source of truth and building trust in each layer before adding the next one.
