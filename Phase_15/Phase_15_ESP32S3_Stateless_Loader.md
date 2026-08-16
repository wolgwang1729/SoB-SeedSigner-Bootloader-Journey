# Phase 15: ESP32-S3 Stateless Secure Loader Port

- **Date:** August 15, 2026
- **Author:** Mayank (wolgwang)
- **Project:** SeedSigner Secure Boot — Summer of Bitcoin

## 1. Overview

Phases 9–14 delivered the stateless secure boot chain on the **ESP32-P4** (Waveshare board): Secure Boot V2 (Layer 1) → SD-card Specter bundle load → secp256k1 multisig verify (Layer 2) → flash-fill anti-phishing proof → `JMP` handoff → payload executing entirely from PSRAM. Phase 15 ports the whole chain to the **ESP32-S3**, proving the architecture is portable across ESP32 families rather than P4-specific.

```
ESP-IDF 2nd-stage bootloader (0x0) + SB V2 RSA verify (virtual eFuses)
        →  stateless secure loader (0x20000, app partition)
            - SDMMC 4-bit mount (CLK=36, CMD=35, D0=37, D1=38, D2=33, D3=34)
            - reads /sdcard/seedsigner_esp32s3.bin into PSRAM, unmounts (TOCTOU-safe)
            - Specter bundle verify (platform attr + version + secp256k1 multisig)
            - anti-phishing proof (TRNG fill + SHA-256 → 4 BIP-39 words)
            - segment routing: fake_flash (PSRAM MMU) vs direct copy (IRAM/DRAM/RTC)
        →  JMP[1..5] handoff (IRAM JMP zone, shared I/D MMU table remap)
        →  stock hello-world payload boots from PSRAM through the stateless_shim
```

## 2. What changed: P4 → S3

This section is the heart of the document. It catalogs every change required to port the ESP32-P4 stateless loader to the ESP32-S3, organized by subsystem. Each entry explains *what* changed, *why* it had to change, and the specific code/config involved.

### 2.1 CPU Architecture: RISC-V → Xtensa LX7

| Aspect | ESP32-P4 (RISC-V) | ESP32-S3 (Xtensa LX7) |
|---|---|---|
| **ISA** | RV32IMACZicsr | Xtensa LX7 with register windowing |
| **Interrupt masking** | `csrw mie, zero` | `rsil a2, 15` (mask all levels) |
| **Memory fence** | `fence.i` | `isync` |
| **Stack switch (loader JMP zone)** | `mv sp, t0` (in naked asm) | `asm volatile("mov a1, %0" :: "r"(sp_top))` — **must be inline asm**, not a register-bound local (see §3) |
| **PMP/memprot teardown** | Clear CSRs `pmpcfg0..3`, `pmpaddr0..15` | Disable `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE` at build time; PMS registers opened via `esp_cpu_configure_region_protection()` |
| **WDT teardown** | Neutralize assist-debug HW stack guard (`0x3FF06000`), clear SWD/LP_WDT/MWDT0/MWDT1/RWDT, clear SYSTIMER | Clear watchpoints, disable RWDT/MWDT0/MWDT1 via `wdt_hal_disable()`, silence SYSTIMER + timer-group IRQs |
| **Vector table handoff** | `esp_cpu_intr_set_ivt_addr()` + `esp_cpu_intr_set_xtvt_addr()` (CLIC IVT + MTVT) | `esp_cpu_intr_set_ivt_addr()` only (`VECBASE`, no CLIC on Xtensa) |

### 2.2 Memory Map: Unified L2MEM → Aliased IRAM/DRAM

This is the single biggest architectural difference.

| | ESP32-P4 | ESP32-S3 |
|---|---|---|
| **Internal SRAM** | HP L2MEM `0x4FF00000–0x4FFBFFFF` (768 KB, **unified** I/D address) | IRAM `0x40370000–0x403E0000` ↔ DRAM `0x3FC88000–0x3FCF0000` — **physical aliases** (same cells, two bus views, offset `0x6F0000`) |
| **Flash cache window** | Shared I/D at `0x40000000–0x44000000` | IROM `0x42000000`, DROM `0x3C000000` (separate virtual ranges, **same MMU table**) |
| **PSRAM cache window** | `0x48000000–0x4C000000` | Same windows as flash — selected per-entry via `SOC_MMU_ACCESS_SPIRAM` (BIT15) |

**Consequences for the loader:**

1. **IRAM only accepts 32-bit word stores.** On the P4, segments were copied byte-by-byte (`d[j] = s[j]`) regardless of target address — L2MEM supports arbitrary-width access. On the S3, any byte store (`s8i`) to IRAM0 `0x40370000+` triggers a hardware `LoadStoreError`. The loader's copy loop must **branch on the destination address** and use `volatile uint32_t *` word stores for IRAM targets:

    ```c
    if (dest_addr >= 0x40370000 && dest_addr < 0x403E0000) {
        // IRAM0: 32-bit word stores only
        volatile uint32_t *d32 = (volatile uint32_t *)dest_addr;
        for (uint32_t j = 0; j < len; j += 4) {
            uint32_t word = src[j] | (src[j+1] << 8) | ...;
            d32[j / 4] = word;
        }
    } else {
        // DRAM / RTC: byte-copy is fine
        uint8_t *d = (uint8_t *)dest_addr;
        for (uint32_t j = 0; j < len; j++) d[j] = src[j];
    }
    ```

2. **RTC fast memory segment.** S3 app images carry a 32-byte `RTC_DATA` segment at `0x50000000`. The segment router adds `[0x50000000, 0x50020000)` as a direct-copy range (not present on P4).

### 2.3 MMU: Separate I/D Tables → Single Shared Table

| | ESP32-P4 | ESP32-S3 |
|---|---|---|
| **Flash MMU** | `SPI_MEM_C_MMU_ITEM_INDEX/CONTENT` registers (I-side) | One shared table at `DR_REG_MMU_TABLE` = `0x600C5000` |
| **PSRAM MMU** | `SPI_MEM_S_MMU_ITEM_INDEX/CONTENT` registers (D-side) | Same table — PSRAM entries set `BIT15` |
| **Programming** | Write index reg, write content reg, per-table | Direct 32-bit store: `*(volatile uint32_t *)(DR_REG_MMU_TABLE + entry * 4) = value` |
| **Entry calculation** | Per-window page index | `entry = (vaddr & 0x1FFFFFF) >> 16` (linear address, 64 KB pages) |
| **Access type** | Bits 10 & 11 set (valid + access) | `SOC_MMU_ACCESS_SPIRAM` = `BIT15` |

So a payload's `.text` at `0x42000020` (linear `0x20`) and `.rodata` at `0x3C020020` (linear `0x20020`) land at **entries 0 and 2** of the same table. The loader programs both windows with the same PSRAM physical page numbers.

### 2.4 Cache: L1 Write-Through + Eviction → Write-Back + Uncached SRAM

| | ESP32-P4 | ESP32-S3 |
|---|---|---|
| **Internal SRAM caching** | L2MEM is **behind L1 D-cache** → dirty lines must be explicitly drained after segment copies (64 KB `evict_buf`, 256 KB post-copy drain) | IRAM/DRAM is **uncached** → plain stores land in physical SRAM directly. **No `evict_buf` needed.** |
| **PSRAM staging** | `Cache_WriteBack_Addr(0x10/0x20, ...)` on both I and D sides | D-cache is **write-back** → `esp_cache_msync(fake_flash, len, C2M \| INVALIDATE)` before remap |
| **Post-remap invalidation** | `Cache_Invalidate_Addr()` per address range + `fence.i` | `Cache_Invalidate_ICache_All()` + `Cache_Invalidate_DCache_All()` (whole-cache flush) + `isync` |
| **ROM bug** | None | `ESP_ROM_HAS_CACHE_WRITEBACK_BUG = 1` — ROM `Cache_WriteBack_Addr` is buggy on S3. Use `esp_cache_msync()` from the IDF API instead. |

**Net effect:** The S3 path is *simpler* (no eviction buffer machinery), but you must use `esp_cache_msync()` instead of the ROM writeback call.

### 2.5 JMP Zone Linker Script

Both platforms relocate the JMP zone above the payload's copy targets, but the details differ due to the IRAM/DRAM alias:

| | ESP32-P4 | ESP32-S3 |
|---|---|---|
| **Loader SRAM** | `sram_low` relocated to `0x4FF40000` (above payload at `0x4FF00000`) | `iram0_0_seg` redefined at `0x40374000, len = 0x2C000` (stock IRAM, ends at `0x403A0000`) |
| **JMP zone** | Within `sram_low` (same unified memory) | Separate `jmp_zone` region: `org = 0x403A0000, len = 0x18000` (high IRAM, DRAM alias at `0x3FCB0000`) |
| **JMP BSS** | In `sram_low` (same region) | Separate `jmp_bss` region: `org = 0x3FC98000, len = 0x38000` (DRAM side, avoids the payload's `.dram0.data` ending around `0x3FC95xxx`) |

**Xtensa-specific linker constraints (new for S3):**

- `.jmp_zone.literal` **must precede** `.jmp_zone.text` — Xtensa `l32r` loads a literal from a *preceding* address within a 256 KB window; placing literals after code causes a fatal link error. RISC-V has no such constraint.
- `.jmp_zone.bss` must be `(NOLOAD)` — all its statics must be uninitialized. A `= 0` initializer triggers "section type conflict."
- **DIRAM alias guard:** `ASSERT(_jmp_zone_bss_end <= 0x3FCB0000)` ensures the JMP BSS variables don't collide with the JMP zone's code (which occupies the same physical SRAM, just at its IRAM alias `0x403A0000`).
- **Stack vs variables ordering:** `jump_stack[16384]` is placed *first* in `.jmp_zone.bss`, followed by a `0x400` gap for Xtensa register-window overflow spills, then the state variables (`safe_mappings`, `safe_copies`, etc.). This prevents a window overflow from corrupting the staging arrays.

### 2.6 Heap Budget: Keeping Internal Heap Off the JMP Zone

The JMP zone's DRAM alias is `0x3FCB0000–0x3FCC8000`. If the internal heap hands out pages in that range, a FreeRTOS allocation corrupts the JMP zone.

**Solution (S3-specific):**
```
CONFIG_ESP32S3_USE_FIXED_STATIC_RAM_SIZE=y
CONFIG_ESP32S3_FIXED_STATIC_RAM_SIZE=0x40000
```

This caps `dram0_0_seg` at `0x3FC88000 + 0x40000 = 0x3FCC8000`, so `_heap_start` begins *exactly where the JMP zone's alias ends*. Free internal heap ≈ 137 KB, ample for SDMMC + FATFS + secp256k1.

The P4 had no equivalent concern — its L2MEM is unified, so the JMP code didn't have a conflicting data alias.

### 2.7 SD Card: On-Chip LDO → External 3.3 V

| | ESP32-P4 | ESP32-S3 |
|---|---|---|
| **Power** | On-chip LDO4 (`sd_pwr_ctrl_new_on_chip_ldo`) | External 3.3 V (no power management call) |
| **Pins** | CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42 | CLK=36, CMD=35, D0=37, D1=38, D2=33, D3=34 |
| **Pull-ups** | `SDMMC_SLOT_FLAG_INTERNAL_PULLUP` | Same flag |
| **Filename** | `seedsigner_esp32p4.bin` | `seedsigner_esp32s3.bin` |

### 2.8 Payload Shim: `stateless_shim.c` + `entry.S`

| | ESP32-P4 | ESP32-S3 |
|---|---|---|
| **Entry point setup** | 7-line inline RISC-V asm in `stateless_shim.c`: set `mstatus`, load `gp`, set `sp`, `tail __wrap_call_start_cpu0` | Separate `entry.S` file with Xtensa windowed-ABI setup: reset `WINDOWBASE`/`WINDOWSTART`, set up 48-byte save area, configure `PS` with `WOE` and `INTLEVEL_MASK`, `call4` to C |
| **`entry.S` file** | **Does not exist** — RISC-V has no register windows | **Required** — Xtensa windowed ABI demands proper frame setup before any C call |
| **Wrapped symbols** | 7 wraps: `cache_hal_init`, `mspi_timing_flash_tuning`, `esp_psram_chip_init`, `bootloader_flash_update_id`, `image_process`, `esp_rtc_init`, `esp_rom_output_tx_wait_idle`, `esp_rom_uart_set_clock_baudrate` | 1 wrap: `cache_hal_init` only — the S3 boot path touches fewer subsystems before `esp_startup_start_app()` |
| **BSS ranges** | Split: `_bss_start_low`/`_bss_end_low` + `_bss_start_high`/`_bss_end_high` + `_iram_bss` | Single: `_bss_start`/`_bss_end` + `_rtc_bss_start`/`_rtc_bss_end` |
| **Vector table** | `esp_cpu_intr_set_ivt_addr` + `esp_cpu_intr_set_xtvt_addr` (CLIC IVT + MTVT tables) | `esp_cpu_intr_set_ivt_addr` only (Xtensa `VECBASE`) |
| **Interrupt routing** | `interrupt_clic_ll_route(0, i, 0)` loop to clear the CLIC matrix | Not needed — Xtensa uses built-in interrupt levels |
| **PSRAM heap** | Manual `heap_caps_add_region_with_caps()` for `0x48800000–0x4A000000` (skipping first 8 MB) | **Deliberately disabled**: payload `sdkconfig.defaults` omits `CONFIG_SPIRAM` entirely, so `esp_psram_is_initialized()` stays false and the heap allocator never tries to claim the PSRAM pages backing `fake_flash` |

### 2.9 Partition Table

| | ESP32-P4 (Phase 13) | ESP32-S3 (Phase 15) |
|---|---|---|
| **Partition table offset** | `0x20000` (128 KB for bootloader) | `0x10000` (64 KB — S3 SB V2 bootloader is `0xA000`) |
| **Factory app** | `0x30000`, 1 MB | `0x20000`, 2 MB |
| **Payload (flash dev path)** | Not present (SD-only in Phase 12+) | `0x220000`, 3 MB, data/`0xFF` — raw `0xE9` image fallback for bring-up without SD wiring |
| **random_fill** | `0x132000`, ~6.8 MB | `0x520000`, ~2.9 MB (moved to make room for payload partition) |

### 2.10 `sdkconfig.defaults`

Key differences from the P4 config:

| Setting | ESP32-P4 | ESP32-S3 | Why |
|---|---|---|---|
| `IDF_TARGET` | `esp32p4` | `esp32s3` | — |
| `PARTITION_TABLE_OFFSET` | `0x20000` | `0x10000` | S3 SB V2 bootloader fits in 64 KB |
| `SPIRAM_SPEED` | `80M` | `40M` | 80 MHz causes MSPI timing failures on DevKitC N8R8 |
| `ESP32S3_INSTRUCTION_CACHE_SIZE` | N/A | `0x4000` (16 KB) | Must match payload; determines IRAM base (`0x40370000 + cache_size`) |
| `ESP32S3_USE_FIXED_STATIC_RAM_SIZE` | N/A | `y` (`0x40000`) | Caps heap below JMP zone's DRAM alias (§2.6) |
| `ESP_SYSTEM_PMP_IDRAM_SPLIT` | `n` | N/A | P4 uses PMP; S3 uses PMS (`MEMPROT_FEATURE`) |
| `ESP_SYSTEM_MEMPROT_FEATURE` | N/A | `n` | S3 PMS blocks D-writes to IRAM0 and execution above `_iram_text_end`; must be disabled for JMP zone |

### 2.11 Build System (`CMakeLists.txt`)

Only one addition: `bootloader_support` added to `PRIV_REQUIRES` — needed for `esp_cache_msync()` (the `esp_cache.h` API lives in the `bootloader_support` component on S3). The P4 used ROM cache functions directly and didn't need this dependency.

## 3. The `jump_stack` dead-strip bug (found and fixed)

The JMP zone switches to a dedicated 16 KB `jump_stack` so the payload boots on a clean, reserved stack. The P4 code did:

```c
register char *sp __asm__("sp");
sp = (char *)(jump_stack + sizeof(jump_stack));
```

On the P4's RISC-V GCC this survives; on the **Xtensa GCC 14.2.0** used for the S3 it is a **dead store**: the register-bound local is never *read* afterwards, so the backend eliminated both the store and — since `jump_stack` then had no references — the whole 16 KB array. The symptom was silent: the build succeeded, `jump_stack` was absent from the symbol table (`.jmp_zone.bss` was only `0x1F8` instead of `0x41F8`), and the payload would have booted on the loader's main-task stack.

**Fix** — make the switch an explicit, un-eliminable asm statement:

```c
asm volatile ("mov a1, %0" :: "r"((uint32_t)(jump_stack + sizeof(jump_stack))));
```

Verified in the ELF: the literal pool holds the correct stack-top address, and `do_mmu_mapping_and_jump` emits `l32r a8, ...; mov.n a1, a8`.

## 4. Summary: Porting Checklist

For anyone porting this loader to another ESP32 variant, here is the consolidated list of things that must change:

| # | Category | Change |
|---|---|---|
| 1 | **ISA** | Replace all inline asm (interrupt masking, stack switch, memory fences) with target ISA equivalents |
| 2 | **Memory map** | Update all address ranges (IRAM, DRAM, PSRAM, RTC, UART) to match the target SoC TRM |
| 3 | **Segment copy** | If internal SRAM has bus-width restrictions (e.g., S3 IRAM = 32-bit only), add a branching copy loop |
| 4 | **MMU** | Rewrite MMU programming to match the target's table layout (shared vs separate, register vs memory-mapped) |
| 5 | **Cache** | Adapt write-back/invalidation calls to the target's cache topology; use IDF APIs if ROM functions are buggy |
| 6 | **JMP zone linker script** | Relocate above payload copy targets; respect ISA constraints (Xtensa literal placement, NOLOAD sections) |
| 7 | **Heap budget** | If JMP zone has a data alias, cap the heap to exclude it (`FIXED_STATIC_RAM_SIZE` or equivalent) |
| 8 | **Memory protection** | Disable or reconfigure memprot/PMP so the loader can write to the payload's IRAM and execute the JMP zone |
| 9 | **SD card** | Update pin assignments and power management to match the dev board |
| 10 | **Payload shim** | Write a target-ISA `entry.S` (or inline asm entry); adjust wrapped symbols to the set that the target's boot path actually calls |
| 11 | **Partition table** | Adjust offsets for the target's bootloader size; add/remove flash-payload dev partition as needed |
| 12 | **sdkconfig** | Set target, cache size, PSRAM speed, memprot, fixed RAM size |

## 5. Partition Layout & Secure Boot V2

| addr | contents |
|---|---|
| `0x0` | bootloader.bin (SB V2 RSA-signed, `0xA000`) |
| `0x10000` | partition table |
| `0x11000` | nvs, data/`0x99`, `0x6000` (anti-phishing digest) |
| `0x17000` | phy_init, `0x1000` |
| `0x18000` | efuse (virtual eFuses, `KEEP_IN_FLASH`) |
| `0x20000` | factory app, `0x200000` — the loader (`seedsigner_secure_loader.bin`) |
| `0x220000` | payload, data/`0xFF`, `0x300000` (3 MB) — dev/bring-up raw image |
| `0x520000` | random_fill, data/`0x06`, `0x2E0000` (~2.9 MB, TRNG fill) |

Secure Boot V2 (RSA-3072, same `secure_boot_signing_key.pem` as Phase 9) stays **virtual** (`CONFIG_EFUSE_VIRTUAL=y` + `KEEP_IN_FLASH`) so everything is reversible on real silicon.

## 6. Boot Sequence (S3 JMP[1..5])

```
JMP[1] entered                        rsil a2,15 (IRQs off); watchpoints cleared;
                                      RWDT/MWDT0/MWDT1 disabled; SysTick+TIMER IRQs silenced;
                                      stack switched to jump_stack (mov a1, …)
JMP[2] WDT and SysTick disabled       D-cache write-back of the PSRAM staging buffers
JMP[3] copying direct segments        IRAM via 32-bit word stores; DRAM/RTC via byte copy
JMP[4] programming MMU, entries=…     shared table at 0x600C5000: entry = linear>>16,
                                      val = (paddr>>16)|BIT15; ICache+DCache invalidated
JMP[5] JUMP!                          UART TX drained → jump to entry point
```

vs P4's JMP[1..8]: the S3 flow is shorter because (a) no L1 D-cache eviction buffer needed for uncached SRAM, and (b) no PMP register clearing — memprot is disabled at build time.

## 7. Testing & Scripts

- `Phase_15/build_payload.sh` — build + sign the shim payload → `Phase_15/build/seedsigner_esp32s3.bin` (SD card). Also exports the RAW image → `Phase_15/build/hello_world_esp32s3_raw.bin` for the flash dev path.
- `Phase_15/flash_payload.sh` — dev/bring-up helper: flashes the RAW shim image into the `payload` partition @ `0x220000`.
- `seedsigner_bootloader_esp32s3_stateless_os/run.sh` — build + flash bootloader/partition-table/loader app, virtual-eFuse pre-flight guard. Payload is **not** flashed — the production flow boots it from the FAT32 SD card.
- `test.sh` + `pytest_seedsigner_bootloader_esp32s3_stateless_os.py` — the Phase 13 P4 harness adapted to the S3 flow: JMP[1..5], entry-point-in-IRAM assertion, `cache_hal_init` interceptor, `esp32s3` chip strings, anti-phishing word stability across reboot. **Requires the physical board** (pytest-embedded, serial).

## 8. Dev/Bring-up Path: Flash `payload` Partition (No SD Card)

The S3 dev kit's SD wiring is not soldered yet. To prove the loader → JMP → PSRAM execution chain *before* that hardware exists, the loader gains a **raw-image fallback from flash** (Phase 11 style), strictly segregated from the production path:

- `partitions.csv` adds `payload, data, 0xFF, 0x220000, 0x300000` (3 MB).
- `main()` mounts the SD card first. If `mount_storage_sdcard()` returns NULL, it calls `load_flash_payload()`: reads the raw `0xE9` image into PSRAM.
- **Verification is deliberately skipped on this path.** A raw image on the **SD** card is still rejected and halts. The flash path exists purely to unblock hardware bring-up.
- Everything downstream (D-cache write-back, segment parse, MMU staging, JMP[1..5]) is shared.

Bring-up sequence: `./build_payload.sh` → `./flash_payload.sh` → `run.sh` → reset → `[FLASH PARTITION] Loaded … bytes` → JMP[1..5] → shim → hello-world.

## 9. Silicon Bring-up Results

Successfully verified on physical **ESP32-S3-DEV-KIT-N8R8** (ESP32-S3 QFN56 v0.2, 8 MB Octal PSRAM) using the flash payload dev fallback (@ `0x220000`):

- Anti-phishing TRNG proof verified (4 BIP-39 words printed consistently across reboots).
- JMP zone hand-off, shared MMU table remap, DROM/IROM PSRAM execution, and FreeRTOS main-task startup all ran to completion.
- Hello-world payload executed the full countdown and cleanly restarted via `esp_restart()`.
- Free heap at `app_main`: 392 KB.

## 10. Next Milestones

1. **MicroPython payload** — port the MicroPython builder overlay to the S3 shim (CLIC vector-table fix does not apply; the S3 uses Xtensa interrupts natively) and sign `micropython.bin`.
2. **SD card hardware** — solder external SD wiring to verify the full multi-signature Specter bundle path on hardware.
3. **Board-side pytest** — run `test.sh` on real silicon, fix any string/behavior drift against the actual boot log.
4. **Portability synthesis** — fold the S3 port into the standalone `wolgwang1729/seedsigner-esp32-bootloader` repo so the shared core targets both families.
