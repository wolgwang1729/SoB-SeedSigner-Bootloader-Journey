# SeedSigner Secure Boot R&D

**Summer of Bitcoin 2026**
**Author:** Mayank Yadav (wolgwang)
**Project:** MicroPython Port R&D for Secure Boot and Removable Storage

## Overview

This repository contains foundational Research & Development for porting SeedSigner to the ESP32 platform. The primary focus of this project is exploring and prototyping a generalized secure bootloader approach capable of validating firmware directly from a removable SD card, ensuring **stateless execution** on the ESP32. 

By collaborating with developers from related projects like Kern and Specter-DIY, the goal is to build a shared verification core that can run on ESP32 while supporting project-specific needs.

## Architecture: Hybrid Secure Boot

This project implements a **Hybrid Architecture** to meet SeedSigner's strict security and statelessness requirements:

1. **Layer 1 (Hardware Root of Trust):** Uses Espressif's native Secure Boot v2 (ECDSA-P256) to cryptographically lock a custom 2nd-stage bootloader in flash via hardware eFuses.
2. **Layer 2 (Software Verification & Stateless Execution):** Chainloads an App-Based 3rd stage loader that mounts the SD card, verifies the payload using a Bitcoin-native `secp256k1` multisig, and uses **Cache MMU Hijacking** to map and execute the payload entirely from volatile RAM (PSRAM) without ever writing the application firmware to internal flash.

## Repository Structure

The R&D process is thoroughly documented and divided into chronological phases:

* **[`LiteratureReview.MD`](./LiteratureReview.MD):** A detailed analysis of the Specter-DIY bootloader, ESP-IDF internals, and Kern architecture.

### Phases
* **[Phase 1: Minimal Secure Boot Prototype](./Phase_01/Phase_1_Prototype.md)**
  Setting up ESP-IDF and enabling Secure Boot v2 using the official ESP32-S3 QEMU emulator. Demonstrates Trust-On-First-Use (TOFU) eFuse burning and failure modes.
* **[Phase 2: SD Card Boot & Stateless Execution](./Phase_02/Phase_2_SD_Card_Boot.md)**
  Development of the App-Based Secure Loader. Implements anti-TOCTOU SD card logic, `secp256k1` signature verification, and the Cache MMU Hijack technique to route instruction fetches to PSRAM.
* **[Phase 3: Hardware Secure Boot Integration](./Phase_03/Phase_3_Hardware_Secure_Boot.md)**
  Merging Layer 1 (Secure Boot v2) and Layer 2 (App-Based Loader) into a complete, emulated end-to-end boot flow.
* **[Phase 4: Loading SeedSigner Firmware](./Phase_04/Phase_4_Loading_SeedSigner_Firmware.md)**
  Adapting the bootloader pipeline to load the actual MicroPython payload. Covers deep architectural fixes for IRAM/DRAM access violations, hidden MMU misalignments, and stack self-overwrite bugs.
* **[Phase 5: Environment Upgrade & MicroPython Execution](./Phase_05/Phase_5_Environment_Upgrade_and_MicroPython_Execution.md)**
  Upgrading to ESP-IDF v6.0.1 and successfully booting the official MicroPython build for the ESP32-S3 via the stateless Secure Loader.
* **[Phase 6: UI Emulation & Validation via Wokwi](./Phase_06/Phase_6_UI_Emulation_and_Validation_via_Wokwi.md)**
  Visually verifying the SeedSigner UI using Wokwi’s virtual hardware emulation. Resolving PSRAM mismatch, bypassing bootloader limitations, and executing the raw MicroPython OS to a fully interactive REPL prompt on the ESP32-S3 architecture.
* **[Phase 7: Flash Hardening](./Phase_07/Phase_7_Flash_Hardening.md)**
  Threat analysis of the stateless boot path on the ESP32-S3 QEMU prototype: seed stashing in flash, brute-force attacks, and the flash-fill mitigation. Introduces the anti-TOCTOU and secret-stashing protections carried into later phases.
* **[Phase 8: Hardware Bring-up](./Phase_08/Phase_8_Hardware_Bring_up.md)**
  Moving the secure loader off emulation onto real ESP32-S3 silicon: hardware bring-up, virtual eFuse secure-boot testing without physical burns, and validation of the stateless handoff on hardware.
* **[Phase 9: ESP32-P4 Bring-up](./Phase_09/Phase_9_ESP32P4_Bring_up.md)**
  Porting the stateful and stateless loaders to the ESP32-P4 (RISC-V). RISC-V boot flow, P4 cache/MMU differences, and running the secured loader on the Waveshare ESP32-P4 board.
* **[Phase 10: ESP32-P4 PSRAM Handoff](./Phase_10/Phase_10_ESP32P4_PSRAM_Handoff.md)**
  First stateless handoff on the ESP32-P4: Cache MMU remap of IROM/DROM to PSRAM physical pages, eviction ordering, and booting a hello-world payload entirely from PSRAM.
* **[Phase 11: MicroPython on App Loader](./Phase_11/Phase_11_MicroPython_on_App_Loader.md)**
  Booting the official MicroPython build statelessly through the loader. Introduces the reusable `stateless_shim` and the CLIC vector-table fix required for shim-based payloads on the P4.
* **[Phase 12: Specter Secure App Loader on Stateless Boot](./Phase_12/Phase_12_Specter_Secure_App_Loader_on_Stateless_Boot.md)**
  Loading firmware from a FAT32 SD card in the Specter `bl_section_t` bundle format with secp256k1 multisig verification, plus the build/sign tooling for wrapping hello-world and MicroPython payloads.
* **[Phase 13: Secure Bootloader Hardening](./Phase_13/Phase_13_Secure_Bootloader_Hardening.md)**
  Anti-phishing hardening of the loader: TRNG flash-fill of a dedicated partition hashed into BIP-39 words printed at each boot, plus a production hardening plan (HMAC eFuse binding, anti-rollback, NVS/flash encryption, key rotation, lockdown).

## Testing & Emulation

To prevent permanently bricking physical development boards by irreversibly blowing eFuses during early development, the majority of this R&D was conducted using the **QEMU emulator** configured for the `esp32s3` machine type. 

*Note: Phases 1–7 were developed on QEMU/Wokwi emulation (ESP32-S3) to avoid permanently burning eFuses during early R&D. The transition to physical bare-metal hardware happened in the second half of the program: Phase 8 brought the loader up on real ESP32-S3 silicon, and Phases 9–13 run on a Waveshare ESP32-P4 board (target config `WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43`, but dev setup is headless — no LCD connected). Secure-boot eFuse writes stay virtual (`CONFIG_EFUSE_VIRTUAL=y`), so the trust-on-first-use flow is testable on real chips without one-time physical burns.*
