# Phase 16: MicroPython on ESP32-S3 Stateless Secure Bootloader

- **Date:** August 15, 2026
- **Author:** Mayank (wolgwang)
- **Project:** SeedSigner Secure Boot — Summer of Bitcoin

## 1. Overview

Phase 15 established the stateless secure bootloader architecture on the **ESP32-S3** (Xtensa LX7), proving the core handoff with a stock `hello_world` payload. **Phase 16** brings the full **SeedSigner MicroPython runtime environment** onto the ESP32-S3 stateless secure loader.

The firmware executes statelessly from external PSRAM via Cache MMU remap (`0x600C5000`), boots through a dedicated Xtensa windowed assembly shim (`entry.S`), initialises the MicroPython virtual machine on CPU0, and runs the SeedSigner application stack.

```
ESP-IDF 2nd-stage bootloader (0x0) + Secure Boot V2 RSA verify (virtual eFuses)
        │
        ▼
stateless secure loader (0x20000, factory app partition)
        - Mounts storage (FAT32 SD card /sdcard/seedsigner_esp32s3.bin or flash dev fallback @ 0x220000)
        - Specter bundle verification (platform=seedsigner_esp32s3, secp256k1 multisig)
        - Anti-phishing proof (TRNG flash fill + SHA-256 → 4 BIP-39 words displayed)
        - Stages IROM/DROM into PSRAM fake_flash; copies direct IRAM/DRAM/RTC segments
        - JMP[1..5] handoff (IRAM JMP zone, shared MMU table remap @ 0x600C5000)
        │
        ▼
entry.S (Xtensa windowed ABI setup)
        - Masks interrupts (PS_INTLEVEL_MASK)
        - Resets window state: WINDOWBASE=0, WINDOWSTART=1
        - Sets stack pointer on custom_boot_stack (16 KB in .dram0.data) with 48-byte save area
        - Sets PS (PS_WOE | PS_INTLEVEL_MASK), calls __wrap_call_start_cpu0
        │
        ▼
stateless_shim.c (Early boot hand-off)
        - Disables RWDT, MWDT0, MWDT1
        - Clears .bss (_bss_start to _bss_end, _rtc_bss_start to _rtc_bss_end)
        - Installs vector table (esp_cpu_intr_set_ivt_addr -> _vector_table)
        - Cache invalidation (Cache_Invalidate_ICache_All / Cache_Invalidate_DCache_All)
        - Initialises clocks (esp_clk_init, esp_perip_clk_init)
        - Clears interrupt routing matrix (core_intr_matrix_clear)
        - Initialises MMU software mapping context (esp_mmu_map_init)
        - Runs system init functions (stages 0 & 1) and global constructors
        - Injects PSRAM heap memory (heap_caps_add_region_with_caps)
        - Jumps to esp_startup_start_app()
        │
        ▼
MicroPython VM Boot
        - __wrap_uart_stdout_init() wires up UART0 RX interrupt without resetting UART clock
        - MicroPython app_main() starts FreeRTOS task on CPU0 (MP_TASK_COREID=0)
        - seedsigner_board_startup() initialises display/touch drivers with headless fallback
        │
        ▼
SeedSigner Controller / Interactive MicroPython REPL
```

## 2. Technical Architecture & Handoff Design

### 2.1 Xtensa Windowed ABI Reset (`entry.S`)

On Xtensa LX7, function calls utilize a sliding register window mechanism (`call4`, `call8`, `call12`, `entry`, `retw`). When the loader hands off execution, the processor registers reflect whatever state the loader's FreeRTOS task was executing in.

To boot the payload cleanly:
1. All interrupts are masked via `PS.INTLEVEL = 15`.
2. `WINDOWBASE` is reset to `0` and `WINDOWSTART` is reset to `1` (marking only window 0 active).
3. The stack pointer (`a1`) is pivoted to `custom_boot_stack` (a 16 KB buffer located in `.dram0.data`) with a 48-byte window base save area (`SAVE_AREA_OFFSET = 48`, `BASE_AREA_SP_OFFSET = 12`).
4. `a0` is cleared to `0` (indicating the top-level outer frame).
5. Window Overflow Exceptions (`PS_WOE`) are enabled before calling `__wrap_call_start_cpu0` via `call4`.

### 2.2 Early Boot Initialisation (`stateless_shim.c`)

Because the ESP-IDF 2nd-stage bootloader's app loading flow is completely bypassed, the payload must re-implement early-boot duties:
- **Watchdog Neutralization:** Disables `RWDT`, `MWDT0`, and `MWDT1` to prevent hardware watchdogs from firing during startup.
- **BSS Clearing:** `memset` zeroes both `.bss` (`_bss_start` to `_bss_end`) and RTC BSS (`_rtc_bss_start` to `_rtc_bss_end`).
- **Vector Base Installation:** Installs `_vector_table` into `VECBASE` using `esp_cpu_intr_set_ivt_addr(&_vector_table)`.
- **Cache Invalidation:** Calls `Cache_Invalidate_ICache_All()` and `Cache_Invalidate_DCache_All()` followed by `isync` to ensure instruction and data caches fetch fresh contents from the remapped PSRAM pages.
- **Clock & Interrupt Matrix Setup:** Configures system clocks (`esp_clk_init`, `esp_perip_clk_init`) and clears stale interrupt routes inherited from the loader via `core_intr_matrix_clear()`.
- **System Init Stages:** Iterates through `_esp_system_init_fn_array_start` to execute core (Stage 0) and secondary (Stage 1) initialization routines and invokes all global C++ constructors.
- **PSRAM Heap Injection:** Injects unused PSRAM pages (`0x3C300000` to `0x3C800000`, 5 MB) into the ESP-IDF heap allocator via `heap_caps_add_region_with_caps()` so MicroPython's garbage collector has access to external RAM without triggering destructive flash/MSPI re-tuning.

### 2.3 Interactive REPL & UART Console (`__wrap_uart_stdout_init`)

In stock MicroPython, `uart_stdout_init()` calls `uart_hal_init()`, which resets the UART peripheral clock and hardware configuration. Because the bootloader has already configured the UART console (115200 baud, 8N1), resetting it corrupts the serial stream.

The shim wraps `uart_stdout_init()` with `__wrap_uart_stdout_init()`:
```c
void IRAM_ATTR __wrap_uart_stdout_init(void) {
    uart_hal_context_t repl_hal = { .dev = UART_LL_GET_HW(0) };
    esp_err_t err = esp_intr_alloc(uart_periph_signal[0].irq,
        ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_IRAM,
        uart_irq_handler, NULL, NULL);
    if (err != ESP_OK) {
        esp_rom_printf("[P16] uart_stdout_init: esp_intr_alloc failed %d\r\n", err);
        return;
    }
    uart_hal_set_rxfifo_full_thr(&repl_hal, SOC_UART_FIFO_LEN - 8);
    uart_hal_set_rx_timeout(&repl_hal, 10);
    uart_hal_ena_intr_mask(&repl_hal, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
}
```
This preserves the console connection while properly allocating the UART0 RX interrupt handler (`uart_irq_handler`), enabling interactive keystroke input at the `>>>` prompt.

### 2.4 Headless & Peripheral Fault Tolerance

On development boards lacking display and touch hardware:
- **I2C Expansion / PMIC:** Missing hardware responses log warnings without triggering fatal panics.
- **AXS15231B Display Driver:** `board_display_axs15231b_init` wraps SPI bus and panel creation, setting handles to `NULL` if the hardware is absent.
- **LVGL Port & Touch Setup:** `board_init.c` skips touch configuration and custom flush registration when handles are `NULL`, transitioning cleanly to headless execution.

## 3. Builder Integration (`seedsigner-micropython-builder`)

The MicroPython payload is built using [`seedsigner-micropython-builder`](https://github.com/SeedSigner/seedsigner-micropython-builder) via Docker. The integration overlay is structured under `deps/micropython/mods/new_files/ports/esp32/boards/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/`:

| File | Role |
|---|---|
| `stateless_shim/CMakeLists.txt` | Registers component, exports `-Wl,-e,my_entry_point`, `--wrap=cache_hal_init`, `--wrap=uart_stdout_init` |
| `stateless_shim/entry.S` | Xtensa windowed ABI entry point |
| `stateless_shim/stateless_shim.c` | C startup shim + PSRAM heap injection + UART0 RX ISR setup |
| `mpconfigboard.cmake` | Appends `stateless_shim` to `MICROPY_EXTRA_COMPONENT_DIRS` |
| `mpconfigboard.h` | Sets `#define MP_TASK_COREID (0)` (single-core CPU0 execution) |
| `sdkconfig.board` | Aligns I-cache (16 KB), single-core FreeRTOS, disables HW stack guard & INT WDT |

## 4. Build, Sign, and Flash Workflow

### Step 1: Compile MicroPython in Docker
```bash
cd ~/Desktop/seedsigner-micropython-builder
make docker-build-all \
  BOARD=WAVESHARE_ESP32_S3_TOUCH_LCD_35B \
  SS_EMBIT_DIR=$HOME/Desktop/seedsigner-micropython-builder/deps/embit \
  SS_APP_DIR=$HOME/Desktop/seedsigner-micropython-builder/deps/seedsigner
```
Produces: `build/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/micropython.bin`

### Step 2: Sign the Payload
```bash
cd ~/Desktop/SoB/Phase_16
./build_micropython.sh
```
- Generates `Phase_16/build/seedsigner_esp32s3.bin` (Specter-signed bundle for SD card).
- Exports `Phase_16/build/micropython_esp32s3_raw.bin` (RAW dev image).

### Step 3: Flash Dev Payload (Bring-up Path)
```bash
./flash_payload.sh
```
Flashes the raw MicroPython image to the `payload` partition at `0x220000`.

### Step 4: Flash and Run the Stateless Secure Loader
```bash
cd seedsigner_bootloader_esp32s3_stateless_os
./run.sh
```

## 5. Verification on Hardware

### Serial Boot Output
```
I (276) esp_image: Verifying image signature...
I (276) secure_boot_v2: Verifying with RSA-PSS...
I (279) secure_boot_v2: Signature verified successfully!
I (286) boot: Loaded app from partition at offset 0x20000
I (351) esp_psram: Found 8MB PSRAM device
I (1437) SEEDSIGNER_LOADER: SeedSigner Loader — ESP32-S3 PSRAM payload
I (1497) SEEDSIGNER_LOADER: No SD card — falling back to the flash 'payload' partition (dev/bring-up path).
I (1977) SEEDSIGNER_LOADER: [FLASH PARTITION] Loaded 2647744 bytes from 'payload' @ 0x00220000
I (4177) anti_phish: ================================================
I (4177) anti_phish:   ANTI-PHISHING PROOF: talk apple express system
I (4177) anti_phish: ================================================
I (4187) SEEDSIGNER_LOADER: Image OK: 7 segments, entry=0x40389B78
I (4187) SEEDSIGNER_LOADER: PSRAM MMU footprint: 2621440 bytes
I (4637) SEEDSIGNER_LOADER: Jumping to 0x40389B78 ...
=== STATELESS SHIM S3 MICROPYTHON PAYLOAD BOOT OK ===
=== __wrap_call_start_cpu0 ENTERED ===
BSS cleared.
Calling clocks init...
Clocks initialized and interrupt matrix cleared.
Initializing MMU software contexts...
Calling init functions...
Running global constructors...
Manually adding PSRAM to heap...
Jumping to esp_startup_start_app...
E (12421) i2c.master: I2C transaction unexpected nack detected
E (12441) tca9554: esp_io_expander_new_i2c_tca9554(83): Reset failed
E (12461) AXP2101: Init PMU FAILED!
MicroPython v1.27.0-1.gdb4fd73b17.dirty on 2026-08-15; Waveshare ESP32-S3-Touch-LCD-3.5B with ESP32S3
Type "help()" for more information.
>>> print(5 + 6)
11
>>> import sys, gc; print("platform:", sys.platform, "free_mem:", gc.mem_free())
platform: esp32 free_mem: 5174592
>>>
```

> **Note:** The I2C NACK errors for TCA9554 (IO expander) and AXP2101 (PMU) are **expected in headless mode** — the dev board has no display/touch hardware connected. The `board_init.c` headless fallback gracefully absorbs these errors without panicking, and MicroPython starts cleanly with an interactive REPL and ~5 MB of free PSRAM heap.

## 6. Artifacts

- `Phase_16/Phase_16_ESP32S3_MicroPython_Stateless.md` — this phase write-up
- `Phase_16/seedsigner_bootloader_esp32s3_stateless_os/` — S3 Stateless Secure Bootloader with Secure Boot V2, anti-phishing proof, and fallback dev path
- `Phase_16/seedsigner_micropython_builder_changes/` — documentation and exact file copies of the `stateless_shim` overlay for `seedsigner-micropython-builder`
- `Phase_16/build_micropython.sh` — payload packaging and Specter signing script
- `Phase_16/flash_payload.sh` — dev bring-up flash helper script
