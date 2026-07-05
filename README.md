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
* **[Phase 1: Minimal Secure Boot Prototype](./Phase_1/Phase_1_Prototype.md)**
  Setting up ESP-IDF and enabling Secure Boot v2 using the official ESP32-S3 QEMU emulator. Demonstrates Trust-On-First-Use (TOFU) eFuse burning and failure modes.
* **[Phase 2: SD Card Boot & Stateless Execution](./Phase_2/Phase_2_SD_Card_Boot.md)**
  Development of the App-Based Secure Loader. Implements anti-TOCTOU SD card logic, `secp256k1` signature verification, and the Cache MMU Hijack technique to route instruction fetches to PSRAM.
* **[Phase 3: Hardware Secure Boot Integration](./Phase_3/Phase_3_Hardware_Secure_Boot.md)**
  Merging Layer 1 (Secure Boot v2) and Layer 2 (App-Based Loader) into a complete, emulated end-to-end boot flow.
* **[Phase 4: Loading SeedSigner Firmware](./Phase_4/Phase_4_Loading_SeedSigner_Firmware.md)**
  Adapting the bootloader pipeline to load the actual MicroPython payload. Covers deep architectural fixes for IRAM/DRAM access violations, hidden MMU misalignments, and stack self-overwrite bugs.
* **[Phase 5: Environment Upgrade & MicroPython Execution](./Phase_5/Phase_5_Environment_Upgrade_and_MicroPython_Execution.md)**
  Upgrading to ESP-IDF v6.0.1 and successfully booting the official MicroPython build for the ESP32-S3 via the stateless Secure Loader.
* **[Phase 6: UI Emulation & Validation via Wokwi](./Phase_6/Phase_6_UI_Emulation_and_Validation_via_Wokwi.md)**
  Visually verifying the SeedSigner UI using Wokwi’s virtual hardware emulation. Resolving PSRAM mismatch, bypassing bootloader limitations, and executing the raw MicroPython OS to a fully interactive REPL prompt on the ESP32-S3 architecture.

## Testing & Emulation

To prevent permanently bricking physical development boards by irreversibly blowing eFuses during early development, the majority of this R&D was conducted using the **QEMU emulator** configured for the `esp32s3` machine type. 

*Note: Transitioning these prototypes from QEMU to physical bare-metal hardware (e.g., ESP32-S3, ESP32-P4) is planned for the second half of the program.*
