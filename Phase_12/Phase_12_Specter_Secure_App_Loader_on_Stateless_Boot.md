# Phase 12: Specter Secure App Loader on the Stateless Bootloader

- **Date:** August 2, 2026
- **Author:** Mayank (wolgwang)
- **Project:** SeedSigner Secure Boot — Summer of Bitcoin

## 1. Overview

Phase 11 delivered a stateless PSRAM bootloader that executes arbitrary ESP32-P4 images (hello world → MicroPython) from PSRAM with **no flash writes**. Phase 12 closes the security gap by (a) re-locking the loader itself under **Secure Boot V2** (the 2nd-stage bootloader verifies the app loader's RSA signature against the eFuse digest — a malicious loader swap is rejected before it runs; virtual eFuses keep this testable on real silicon) and (b) putting the **Specter-DIY secure app loader** in front of the SD-card jump — the same crypto core (`components/specter_crypto`) that Phase 9 used for its stateful loader, now bolted onto the Phase 11 stateless loader.

The full chain becomes:

```
ESP-IDF 2nd-stage bootloader (0x2000)
        →  stateless secure loader (0x30000)
            - mounts the SD card, reads /sdcard/seedsigner_esp32p4.bin into PSRAM
            - unmounts the SD card immediately (TOCTOU-safe)
            - validates section header + platform attribute + version
            - hashes the main section, builds the Bech32 signature message
            - verifies secp256k1 multisig against the vendor keys
        →  JMP[1..8] stateless handoff (WDT/int/PMP teardown, cache eviction,
           MMU PSRAM remap)  [unchanged from Phase 11]
        →  payload executes entirely from PSRAM
```

**The SD card is the only load source** (SeedSigner's stateless model — swap the SD card to update firmware). The loader mounts a FAT32 card at boot, reads the signed bundle `/sdcard/seedsigner_esp32p4.bin`, **unmounts it before verification** (TOCTOU-safe: verification and execution operate only on the PSRAM-resident copy), and never falls back to flash. The former flash `payload` partition was removed.

## 2. Target

1. Take `Phase_11/seedsigner_bootloader_p4_stateless_os` as the base loader.
2. Port in the Specter secure app loader **only** from the Phase 9 `specter_crypto` component (section format, hashing, Bech32 signature message, secp256k1 multisig verify, syscall stubs). Do **not** port Phase 9's stateful `main.c` SDMMC path yet.
3. Load the payload from the **SD card**: the loader mounts a FAT32 card, reads the Specter-signed `seed.bin` (`/sdcard/seedsigner_esp32p4.bin`) into PSRAM, unmounts it, then verifies + executes — no flash partition, no legacy raw-image fallback.
4. First test target: a hello-world payload (`hello_world_esp32p4_stateless_payload`) — prove the signed bundle boots before moving to bigger payloads. **The payload boots through the reusable `stateless_shim` hand-off** (the same shim the Phase 11 MicroPython build uses, restored from commit `64a9cf7`): `my_entry_point` → `__wrap_call_start_cpu0` → init-fn array → `esp_startup_start_app()`. One fix was required to make the shim work for a plain ESP-IDF app: the shim replaces `call_start_cpu0()`, which is what normally installs the app's CLIC interrupt vector tables, so the shim now calls `esp_cpu_intr_set_ivt_addr`/`esp_cpu_intr_set_xtvt_addr` before jumping into `esp_startup_start_app()`. Without that the payload's tick hooks never fire, the interrupt watchdog re-armed by `esp_int_wdt_init` is never fed, and the chip dies with `rst:0x7 (HP_SYS_HP_WDT_RESET)` (see `Debugging_Notes.md`).
5. Verify the *rejection* path too: a bundle signed with the wrong key, a tampered image, and a missing signature section must all halt before `JMP[1]`.

## 3. What was copied

| Artifact | From | To |
|---|---|---|
| Loader (build system, `main/main.c`, `partitions.csv`, `sdkconfig.defaults`, pytest) | `Phase_11/seedsigner_bootloader_p4_stateless_os` | `Phase_12/seedsigner_bootloader_p4_stateless_os` |
| `specter_crypto` component (bl_section, bl_signature, bl_syscalls, bl_util, bl_integrity_check, secp256k1, sha2, bech32, crc32) | `Phase_9/seedsigner_bootloader_p4_stateless_os/components/specter_crypto` | `Phase_12/.../components/specter_crypto` (nested `.git` stripped) |
| Signing tool `tools/generate_signed_payload.py` + `tools/package_firmware.py` | `Phase_9/.../tools` | `Phase_12/.../tools` |
| Payload `hello_world_esp32p4_stateless_payload` | `Phase_11/hello_world_esp32p4_stateless_payload` | `Phase_12/hello_world_esp32p4_stateless_payload` (boots through the `stateless_shim`; `main/hello_world_esp32p4.c` prints a `PHASE 12` banner) |
| Stock-payload repro `hello_world_esp32p4_stock_shim` | `$IDF/examples/get-started/hello_world` (unmodified) + `components/stateless_shim` | `Phase_12/hello_world_esp32p4_stock_shim` — proves the shim boots an **untouched stock ESP-IDF app**; only edits: `main/CMakeLists.txt` gains `REQUIRES stateless_shim`, `sdkconfig.defaults` copied, and the example's `MINIMAL_BUILD` line removed (see caveat below) |
| Run scripts | Phase 11 `run.sh` | `build_payload.sh` (build + sign payload → `build/seedsigner_esp32p4.bin`), `build_micropython.sh` (sign the externally-built `micropython.bin`), `run.sh` (flash the loader artifacts only — payload-agnostic, the payload boots from the SD card) |
| Secure Boot V2 signing key `secure_boot_signing_key.pem` (RSA) | `Phase_9/.../secure_boot_signing_key.pem` | `Phase_12/.../secure_boot_signing_key.pem` (same key — the eFuse digest matches the bootloader/app builds) |

## 4. Secure boot chain (restored from Phase 9)

Phase 12 builds the full two-layer root of trust, matching Phase 9:

1. **Layer 1 — hardware:** the project is built with **Secure Boot V2** (`CONFIG_SECURE_BOOT=y`, `CONFIG_SECURE_BOOT_V2_ENABLED=y`, RSA scheme, `secure_boot_signing_key.pem`). On the loader's first boot the 2nd-stage bootloader burns the signing-key digest into eFuses, then verifies the **app loader** (`seedsigner_secure_loader.bin`) RSA signature against that digest on **every** boot. A malicious or wrong loader image in flash is rejected before it can run — this is the hardware root of trust protecting the loader itself.
2. **Layer 2 — the loader:** the loader then verifies the SD-card payload via the Specter secp256k1 multisig (below). So the eFuse protects the *code that does the verification*; the SD bundle is verified by that eFuse-protected code.
3. **Virtual eFuses for safe development:** all eFuse writes are simulated (`CONFIG_EFUSE_VIRTUAL=y`, `KEEP_IN_FLASH`, persisted in the `efuse` data partition added to `partitions.csv`) — no physical eFuse is burned on the dev board. `run.sh` runs a pre-flight check and **refuses to flash** unless `CONFIG_EFUSE_VIRTUAL=y` is verified in the loader configs (same guard as Phase 9's `run_test.sh`).

## 5. Integration details

### Loader (`main/main.c`)

The Phase 11 flow is preserved exactly from Step 3 onward (MMU footprint pass, `fake_flash` staging, deferred SRAM copies, the **evict-before-copy** ordering fix from Phase 11, and the `do_mmu_mapping_and_jump()` teardown). The changes are confined to the loading stage:

- **Step 1 (load):** mounts the SD card via the native SDMMC peripheral (on-chip LDO4 power, 4-bit bus, CLK=43/CMD=44/D0=39/D1=40/D2=41/D3=42), `stat`s `/sdcard/seedsigner_esp32p4.bin`, reads the whole file into PSRAM, and **unmounts immediately** (TOCTOU-safe). The file *is* the complete Specter bundle, so `fw_size = st_size` — no header probe, no slack math. A missing card/file halts forever; the legacy raw `0xE9` fallback was **removed** (a non-Specter file on the SD card is wiped and halts).
- **Step 2.5 (verify):** the Specter secure-app-loader sequence, identical to Phase 9's:
  1. `blsect_validate_header()` — magic, struct revision, `struct_crc`.
  2. `blsect_get_attr_str(bl_attr_platform)` must equal `"seedsigner_esp32p4"`.
  3. `pl_ver >= 1` (downgrade check).
  4. `blsect_hash_over_flash()` over the whole main section (header + payload) — `blsys_flash_read` treats the `bl_addr_t` as a direct PSRAM pointer, so no real flash reads happen.
  5. Locate the `"sign"` section at `psram_buf + sizeof(bl_section_t) + pl_size`, build the Bech32 signature message (`blsect_make_signature_message`), and run `blsig_verify_multisig("secp256k1-sha256", ...)` against `vendor_keys`.
  6. Any failure → `memset(psram_buf, 0, fw_size)` (wipe the unverified firmware) and halt forever.
- **payload_offset:** after a successful verify, `payload_offset = sizeof(bl_section_t)` and all image-header/segment parsing and deferred-copy sources are offset by it. `vendor_keys[]` is the Phase 9 test key (dummy secret `123456789`), so the dev-signed `seedsigner_esp32p4.bin` verifies.

### Component (`specter_crypto`)

Copied verbatim from Phase 9 with its `CMakeLists.txt` intact. Notable syscall stubs in `bl_syscalls.c`: `blsys_flash_read` is a plain `memcpy` from the given address (PSRAM), `blsys_flash_write`/`blsys_flash_erase` **return false** (flash writes stay disabled — stateless), and all FatFs/media functions are stubs (`blsys_media_devices` → 0). `secp256k1` is compiled in as a static lib with the Phase 9 compile definitions. The `fatfs` `REQUIRES` is kept for the SD-card read in `main.c`; the Specter `blsys_media_*`/`blsys_f*` abstraction stays inert because the loader reads the card directly with ESP-IDF's `esp_vfs_fat_sdmmc_mount` + POSIX `fopen`/`fread`.

### Payload (`hello_world_esp32p4_stateless_payload`)

A plain ESP-IDF app that boots through the **reusable `stateless_shim`** (the same
component the Phase 11 MicroPython build uses), restored from commit `64a9cf7`:

- `components/stateless_shim/` provides `my_entry_point`, which the app links
  against with `-Wl,-e,my_entry_point`. The shim's `__wrap_call_start_cpu0`
  clears `.bss`, does clock/MMU init, runs the ESP-IDF init-fn array, adds PSRAM
  to the heap, and then calls the real `esp_startup_start_app()`. It also
  `--wrap`s the ESP-IDF early-boot functions that would re-init hardware the
  loader already configured (`cache_hal_init`, `mspi_timing_flash_tuning`,
  `esp_psram_chip_init`, `bootloader_flash_update_id`, ...) as no-ops.
- **Vector-table requirement (the fix that made the shim work for a plain IDF
  app):** in a normal boot, `call_start_cpu0()` → `init_cpu()` installs
  `_vector_table`/`_mtvt_table`. Because the shim replaces `call_start_cpu0`,
  it must do that itself — right before `esp_startup_start_app()` it calls
  `esp_cpu_intr_set_ivt_addr(_vector_table)` and
  `esp_cpu_intr_set_xtvt_addr(_mtvt_table)` (P4 CLIC). Without this the
  FreeRTOS tick hooks (`usb_serial_jtag_sof_tick_hook`, the IWDT feeder) never
  run, so the interrupt watchdog that `esp_int_wdt_init` re-arms inside
  `esp_startup_start_app` is never fed and the chip resets with
  `rst:0x7 (HP_SYS_HP_WDT_RESET)`. Symptom was unmistakable: `Core0 Saved
  PC:0x4ff01440` parked inside `usb_serial_jtag_sof_tick_hook`.
- `main/hello_world_esp32p4.c` defines a plain `app_main` with a `PHASE 12:
  SHIM-BASED STATELESS PAYLOAD` banner and a periodic PSRAM hello task. It runs
  entirely from PSRAM with no flash writes.
- `.data` is copied by the loader, so the payload must not duplicate ESP-IDF's
  `.data` init in the shim.

### Run scripts (`run.sh`, `build_payload.sh`, `build_micropython.sh`)

The build/flash flow is split into two stages. `build_payload.sh` builds the stock hello-world payload and runs `tools/generate_signed_payload.py` to wrap the raw image into a Specter bundle (`main` section + `sign` section), writing `build/seedsigner_esp32p4.bin`; `build_micropython.sh` does the same for the externally-built `micropython.bin`. Both print a reminder to copy `seedsigner_esp32p4.bin` to the FAT32 SD card root. `run.sh` then only builds + flashes the loader artifacts — it is payload-agnostic, so one script serves both payloads and does **not** touch the payload. A pre-flight step verifies `CONFIG_EFUSE_VIRTUAL=y` (aborting the flash otherwise). Flash layout (loader artifacts only):

| addr | contents |
|---|---|
| `0x2000` | bootloader.bin |
| `0x20000` | partition table |
| `0x30000` | `seedsigner_secure_loader.bin` |

The `payload` partition was removed from `partitions.csv`; an `efuse` data partition (0x2000) was added for the virtual-eFuse persistence. The firmware lives on the SD card, not in flash.

## 6. Verification status

- **Builds:** loader and payload both compile clean under ESP-IDF v5.5 (loader now 0x468c0 bytes with secp256k1/sha2 linked; fits the 1 MB factory partition).
- **Signing:** `generate_signed_payload.py` produces a valid bundle (`magic=0x54434553`, `rev=1`, `name="main"`, `pl_ver=1`, `pl_size=187552`), and the printed `vendor_keys[]` matches the key compiled into `main.c`.
- **On-hardware (verified, flash load):** the Specter-signed hello-world bundle boots end-to-end **through the `stateless_shim`**. Serial flow:
  `Found Specter bootloader section header` → `Signature verification PASSED!` → `JMP[1]` … `JMP[8]` → `=== __wrap_call_start_cpu0 ENTERED ===` → init-fn array → `Jumping to esp_startup_start_app...` → `main_task: Calling app_main()` → `PHASE 12: SHIM-BASED STATELESS PAYLOAD` banner → `Hello from a FreeRTOS Task running completely in PSRAM (shim boot)!` every ~1 s (stable, no resets). *(These runs loaded the bundle from the old flash `payload` partition; the same verification code now reads from the SD card.)*
- **Stock-payload repro (verified):** the **untouched** `$IDF/examples/get-started/hello_world` (only `hello_world_main.c`, zero app-side edits) boots through the same shim in `hello_world_esp32p4_stock_shim`. Serial flow: verify → `JMP[8]` → shim hand-off → `Jumping to esp_startup_start_app...` → `Hello world!` → `This is esp32p4 chip ... 8MB external flash` → `Minimum free heap size: 25752152 bytes` → `Restarting in 10 seconds...` countdown → `Restarting now.` (`rst:0xc SW_CPU_RESET`) → full second stateless boot cycle. Zero `HP_SYS_HP_WDT_RESET`, zero panics across repeated `esp_restart()` cycles — proving the shim is drop-in for any plain ESP-IDF app.
- **MicroPython payload (verified, flash load):** the full SeedSigner MicroPython firmware (built by the external `seedsigner-micropython-builder`, 4.5 MB) boots through the Phase 12 secure loader with Specter signature verification. Serial flow: `Found Specter bootloader section header` → `Signature verification PASSED!` → `Image OK: 7 segments, entry=0x4FF0403A` → `JMP[1]` … `JMP[8]` → `=== __wrap_call_start_cpu0 ENTERED ===` → init-fn array → `Jumping to esp_startup_start_app...` → `MicroPython v1.27.0-1...` → `>>>` REPL prompt. REPL input verified: `print('P12_MP_OK')` echoed, `import embit` + `import seedsigner` succeeded (no `ImportError`). No `TRAP!`, no `rst:0x7 HP_SYS_HP_WDT_RESET`. The MP shim needed the same CLIC vector-table fix as the hello-world shim — applied to the builder's `stateless_shim.c` using `esp_cpu_intr_set_ivt_addr` / `esp_cpu_intr_set_mtvt_addr` (see `Debugging_Notes.md` Part 5). Signing the MP payload is handled by `build_micropython.sh`; `run.sh` flashes the loader artifacts and the payload boots from the SD card.
- **SD-card load (verified on hardware):** Step 1 reads `/sdcard/seedsigner_esp32p4.bin` and unmounts before verification. Confirmed serial flow with the MicroPython payload on a FAT32 SD card: `SD card mounted at /sdcard` → `Unmounting SD card before verification (TOCTOU-safe)...` → `[SD CARD] Loaded 4497872 bytes from /sdcard/seedsigner_esp32p4.bin` → `Specter bootloader section detected` → `Performing secp256k1 multisig verification...` → `Signature verification PASSED!` → `Image OK: 7 segments` → `JMP[1]` … `JMP[8]` → `=== __wrap_call_start_cpu0 ENTERED ===` → `MicroPython v1.27.0-1...` → `>>>` REPL prompt. The LFN fix (`CONFIG_FATFS_LFN_HEAP=y`) is what makes the 22-char filename loadable.
- **Secure Boot V2 (verified on hardware):** the loader project builds with Secure Boot V2 + virtual eFuses restored from Phase 9 (RSA `secure_boot_signing_key.pem`, `efuse` partition, `CONFIG_EFUSE_VIRTUAL=y` + `KEEP_IN_FLASH`, `run.sh` pre-flight guard). Confirmed serial flow: the bootloader logs `eFuse virtual mode is enabled... FOR TESTING ONLY!` and `[Virtual] try loading efuses from flash: 0x130000`, verifies the loader with `secure_boot_v2: Verifying with RSA-PSS...` → `Signature verified successfully!` → `secure_boot v2 is already enabled, continuing..` (the key digest was burned **virtually** on the first boot and persists in the `efuse` partition). A flash of an **unsigned/tampered** `seedsigner_secure_loader.bin` must still be rejected (`Error verifying app image`) — worth re-confirming after a deliberate tamper.
- **Debugging history:** three hard-won fixes were needed to get from "jump happens but payload never runs" to the boot above — (1) relocating the loader's own segments off the payload's region (`main/ld/loader_high.ld`) plus the evict-before-copy D-cache drain, and (2) giving the JMP zone a dedicated `jump_stack` clear of the payload SRAM text, with the RISC-V HW stack guard (`assist_debug`) torn down in the naked trampoline, and (3) **the shim vector-table install** (this section). A fourth gotcha (MINIMAL_BUILD dropping the PSRAM layout) bit the stock-payload repro — full detail in `Debugging_Notes.md`. A fifth fix applied the vector-table install to the MicroPython shim (`esp_cpu_intr_set_mtvt_addr` for the container IDF's naming) — see Part 5.

## 7. Next steps

- **Done now:** integrate the Specter secure app loader (secp256k1 multisig verification) into the stateless loader — verified with the signed hello-world bundle, an **unmodified stock ESP-IDF `hello_world` example**, and the full MicroPython firmware, all booting through the same `stateless_shim`.
- **Done now:** the signed bundle is read from the **SD card** (`/sdcard/seedsigner_esp32p4.bin`) instead of a flash partition, with an immediate TOCTOU-safe unmount and the legacy raw-image fallback removed. Hardware-verified: the MicroPython payload boots end-to-end from the SD card through the loader (see Verification status).
- **After that:** production hardening — replace the Phase 9 dummy `vendor_keys[]` with real offline-held keys, and consider ICR/version-rollback records.

## 8. Notes / caveats

- **Test key:** `vendor_keys[]` is the Phase 9 dummy key. A production phase must replace it (and the offline signing key) — documented in `main.c`.
- **MINIMAL_BUILD:** the stock example's `idf_build_set_property(MINIMAL_BUILD ON)` must be **removed** for a shim payload. It drops `esp_psram` from the component set, so the `CONFIG_SPIRAM_*` defaults silently vanish, the app's IROM/DROM stay in the `0x40000000` flash-cache window (read-only), and the loader's direct copy faults (`Store access fault` at `MTVAL=0x40010020` during cache eviction). Without MINIMAL_BUILD the PSRAM XIP layout places IROM/DROM at `0x48000000`, which the loader MMU-maps to PSRAM.
- **SD card is the only source:** the flash `payload` partition is gone from `partitions.csv`, and a non-Specter file (missing `BL_SECT_MAGIC`) on the SD card is wiped and halts. Every boot path is signature-checked.
- **FatFs LFN required:** `seedsigner_esp32p4.bin` (22 chars) exceeds the FAT 8.3 short-name limit, so `CONFIG_FATFS_LFN_HEAP=y` must stay set in `sdkconfig.defaults` — with the FatFs default (`LFN_NONE`) the loader mounts the card but reports `Firmware file /sdcard/seedsigner_esp32p4.bin not found` (see `Debugging_Notes.md` Part 6). Earlier SD reads used 8.3-compatible names (`seed.bin`, `hello.txt`), which is why they worked without it.
- **TOCTOU:** the card is unmounted immediately after `fread` — verification and execution operate only on the PSRAM-resident copy (verify-into-RAM *then* use, per Specter's guidance).
- **Statelessness preserved:** the verify path uses PSRAM + the SD read only; `blsys_flash_write`/`blsys_flash_erase` remain hard-stubbed to `false`, consistent with the Phase 11 flash-write audit.

Artifacts: `seedsigner_bootloader_p4_stateless_os/run.sh` (build + flash loader + monitor; prints the SD-copy reminder), `build_payload.sh` / `build_micropython.sh` (build + sign the payloads), and `seedsigner_bootloader_p4_stateless_os/` (loader + specter component + tools).
