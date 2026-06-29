# Phase 3: Hardware Secure Boot Integration (Emulated)

**Date:** June 28, 2026
**Author:** Mayank (wolgwang)
**Project:** SeedSigner Secure Boot — Summer of Bitcoin

## 1. Summary
This document summarizes the completion of Phase 3, which successfully proves the **Hybrid Architecture** (Option D) end-to-end. I have successfully merged the Layer 1 hardware Root of Trust (ESP-IDF Secure Boot v2) from Phase 1 with the Layer 2 App-Based stateless SD card loader from Phase 2.

To avoid irreversibly bricking physical boards during development, this integration was proven using the ESP32-S3 QEMU emulator. The emulator successfully demonstrated the "Trust On First Use" (TOFU) eFuse burning process, followed by a locked-down reboot that verified the custom bootloader, mounted the simulated SD card, and statelessly executed the payload purely from RAM.

## 2. Configuration & Build Process

I began by duplicating the `seedsigner_bootloader_proto` from Phase 2 and configuring it to be securely signed. 

The `sdkconfig.defaults` was augmented with the Secure Boot v2 configuration:
```ini
CONFIG_SECURE_BOOT=y
CONFIG_SECURE_BOOT_V2_ENABLED=y
CONFIG_SECURE_BOOT_SUPPORTS_RSA=y
CONFIG_SECURE_SIGNED_ON_BOOT=y
CONFIG_SECURE_SIGNED_APPS_NO_RSA_SIGN=n
CONFIG_SECURE_BOOT_SIGNING_KEY="secure_boot_signing_key.pem"
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y
CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME=n
```

Running `idf.py build` successfully generated and appended an RSA-3072 signature block to the `seedsigner_secure_loader.bin` application, ensuring it can be hardware-verified by the 2nd-stage ESP-IDF bootloader.

## 3. The QEMU Merging Gotcha (`dd` vs `esptool.py`)

A critical lesson learned during this phase involved how to prepare the unified 4MB flash image required by QEMU. 

Initially, I attempted to use `esptool.py merge_bin` to combine the partitions. However, `esptool.py` recalculates simple headers and flash headers when stitching binaries. This modification **silently invalidates the RSA-PSS signature** placed on the secure bootloader, causing QEMU to either crash or reject the boot sequence.

To fix this, I abandoned `esptool.py` for the merge and instead utilized `dd` to stitch the files bit-for-bit into an empty 4MB binary without tampering with the cryptographic signatures:

```bash
# Create a 4MB empty file
dd if=/dev/zero bs=1M count=4 of=merged_flash.bin

# Stitch the binaries at their precise offsets
dd if=build/bootloader/bootloader.bin of=merged_flash.bin bs=1 seek=0 conv=notrunc
dd if=build/partition_table/partition-table.bin of=merged_flash.bin bs=1 seek=131072 conv=notrunc
dd if=build/seedsigner_secure_loader.bin of=merged_flash.bin bs=1 seek=196608 conv=notrunc
dd if=build/storage.bin of=merged_flash.bin bs=1 seek=1245184 conv=notrunc
dd if=dummy_fat_dir/seed.bin of=merged_flash.bin bs=1 seek=3342336 conv=notrunc
```

## 4. The Hybrid Boot Sequence (Log Analysis)

When launching QEMU with the correctly stitched image, the ESP-IDF ROM and 2nd-stage bootloader performed perfectly.

### Boot 1: The TOFU eFuse Burn
On the very first boot, the virtual hardware identified that its eFuses were blank. It verified the attached RSA signature, extracted the public key digest, and permanently "burned" it into the eFuse to lock down the chip:
```text
I (198) secure_boot_v2: Secure boot V2 is not enabled yet and eFuse digest keys are not set
I (199) secure_boot_v2: Verifying with RSA-PSS...
I (204) secure_boot_v2: Signature verified successfully!
I (207) secure_boot_v2: enabling secure boot v2...
I (247) secure_boot_v2: Burning public key hash to eFuse
I (249) efuse: Writing EFUSE_BLK_KEY0 with purpose 9
I (381) secure_boot_v2: Secure boot permanently enabled
```

### Boot 2: Hardware Root of Trust
Because eFuses were burned, the device immediately restarted itself. On the second boot, the ROM read the eFuse, cryptographically validated our custom App-Based loader, and handed off execution:
```text
Valid secure boot key blocks: 0
secure boot verification succeeded
```

### Boot 3: The Stateless App Loader
Once our `seedsigner_bootloader_proto` took over, it mounted the FAT partition (representing the SD card), pulled the payload (`seed.bin`), mapped the Cache MMU to PSRAM, and executed the payload without flashing it:
```text
I (8172) SEEDSIGNER_LOADER: Starting SeedSigner Secure Loader (App Mode)
I (8172) SEEDSIGNER_LOADER: Initializing FAT on SPI Flash for QEMU testing...
I (8192) SEEDSIGNER_LOADER: SPI Flash FAT mounted successfully.
I (8202) SEEDSIGNER_LOADER: Loading segment 2: addr=0x40374000, len=101588
I (8422) SEEDSIGNER_LOADER: Jumping to entry point...
```

### Execution: Stateless Payload
Finally, the `hello_world.bin` payload successfully booted purely from volatile RAM:
```text
Hello world!
This is esp32s3 chip with 2 CPU core(s), WiFi/BLE, silicon revision v0.3, Flash size query disabled (PURE RAM APP)
```

## 5. Dummy Payload Verification (secp256k1)

To fully validate Layer 2 (the App-Based Loader), I integrated the Specter Bootloader's custom payload verification logic. 

1. **Packaging**: I created `generate_signed_payload.py` which wraps the raw `hello_world.bin` payload in a 256-byte main section header (containing version, size, and CRC32) and appends a 256-byte signature block at the end of the file.
2. **Signing**: The script utilizes a dummy `secp256k1` key (derived from a simple exponent) to calculate a deterministic signature over a bech32-encoded SHA-256 hash of the main section.
3. **Verification**: I configured the `seedsigner_bootloader_proto` with the corresponding dummy public key in `vendor_keys`. During the boot sequence, the loader parses the FAT filesystem to find `seed.bin`, validates the main section header, and then mathematically verifies the `secp256k1` signature appended at the end of the file before permitting memory mapping and execution.

This successfully proves that our stateless loader can robustly verify cryptographic signatures on the fly before jumping to arbitrary payloads.

## 6. Conclusion & Next Steps

This conclusively proves the SeedSigner Hybrid Secure Boot architecture. The ESP32 hardware root of trust natively protects our custom 3rd-stage loader, which in turn statelessly protects the application firmware.

The next step (Phase 4) is to adapt this pipeline to boot the actual `seedsigner.bin` firmware instead of the `hello_world.bin` placeholder, validating the UI and standard SeedSigner operations from PSRAM.

## 7. QEMU eFuse Persistence (Experiment)

During this phase, I attempted to enable persistent eFuses so QEMU would retain the "blown" Secure Boot state across cold reboots.

1. I created an empty eFuse backing file: `dd if=/dev/zero bs=1 count=4096 of=qemu_efuse.bin`
2. I attempted to attach it via QEMU using `-drive file=qemu_efuse.bin,if=none,format=raw,id=efuse` and `-global driver=nvram.esp32.efuse,property=drive,value=efuse`.
3. **Findings:** 
   - Using `nvram.esp32.efuse` allowed QEMU to execute the emulation successfully, but it silently ignored writing back to the file due to the architecture mismatch.
   - I researched this and found that for the ESP32-S3, the correct driver property is officially `nvram.esp32s3.efuse`. However, when I attempted to use it, the specific QEMU binary I was running (`esp_develop_9.0.0_20240606`) threw an `invalid class name` error.
   
**Conclusion:** Because QEMU intrinsically emulates eFuses in volatile RAM (and retains this RAM during the software soft-reboots triggered by the TOFU process), the Secure Boot emulation works perfectly for validating the bootloader flow in a single session without needing a persistent backing file!
