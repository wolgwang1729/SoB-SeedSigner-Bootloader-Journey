# Phase 16 — SeedSigner MicroPython Builder Changes (ESP32-S3)

Documentation + artifacts for the changes required in
[`seedsigner-micropython-builder`](https://github.com/SeedSigner/seedsigner-micropython-builder)
so it builds MicroPython firmware that boots on top of the Phase 15/16 **ESP32-S3 stateless secure bootloader**
(`seedsigner_bootloader_esp32s3_stateless_os`, sibling folder in `SoB/Phase_16/`).

The custom bootloader executes the app statelessly from PSRAM via Cache MMU table remap (`0x600C5000`),
jumps straight into the app's custom entry point (`my_entry_point` in `entry.S`), and
**never runs the ESP-IDF 2nd-stage bootloader**.

---

## 🧩 Architecture & Flow

```
ESP32-S3 Stateless Bootloader (0x20000)
    │  JMP[1..5] handoff (IRAM JMP zone, shared I/D MMU table remap)
    │  jumps to my_entry_point (-e my_entry_point)
    ▼
entry.S (Xtensa windowed ABI setup)
    │  Masks interrupts (PS_INTLEVEL_MASK)
    │  Resets window registers: WINDOWBASE=0, WINDOWSTART=1
    │  Sets up custom_boot_stack (16 KB in .dram0.data) with 48-byte save area
    │  Configures PS (PS_WOE | PS_INTLEVEL_MASK)
    │  call4 __wrap_call_start_cpu0
    ▼
stateless_shim.c (ESP-IDF early-boot handoff)
    │  Disables watchdogs (RWDT, MWDT0, MWDT1)
    │  Clears .bss (_bss_start to _bss_end, _rtc_bss_start to _rtc_bss_end)
    │  Installs app vector table (esp_cpu_intr_set_ivt_addr)
    │  Flushes and invalidates I-cache and D-cache
    │  Initializes clocks (esp_clk_init, esp_perip_clk_init)
    │  Initializes MMU software mapping context (esp_mmu_map_init)
    │  Executes system init functions (stages 0 & 1) and global constructors
    │  Jumps to esp_startup_start_app()
    ▼
MicroPython VM Boot
    │  __wrap_uart_stdout_init() intercepts UART init to allocate RX ISR without resetting baud
    │  MicroPython app_main() starts FreeRTOS scheduler on CPU0 (MP_TASK_COREID=0)
    ▼
Interactive MicroPython REPL (>>>) / SeedSigner UI
```

---

## 📁 Files & Modifications

All overlay files are mirrored in `files/` at their exact repository paths:

### 1. `stateless_shim/` Component (NEW)
**Path:** `deps/micropython/mods/new_files/ports/esp32/boards/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/stateless_shim/`

- `entry.S` — Xtensa assembly entry point resetting register windows and setting stack save area.
- `stateless_shim.c` — C early boot hand-off, watchdog disable, cache invalidation, and `__wrap_uart_stdout_init()`.
- `CMakeLists.txt` — Registers component and passes `-Wl,-e,my_entry_point`, `-Wl,--wrap=cache_hal_init`, `-Wl,--wrap=uart_stdout_init`.

### 2. `mpconfigboard.cmake` (MODIFIED)
Appends the `stateless_shim` component:
```cmake
list(APPEND MICROPY_EXTRA_COMPONENT_DIRS "${MICROPY_BOARD_DIR}/stateless_shim")
```

### 3. `mpconfigboard.h` (MODIFIED)
Ensures single-core execution:
```c
#define MP_TASK_COREID                      (0)
```

### 4. `sdkconfig.board` (MODIFIED)
Aligns hardware parameters with the Phase 15/16 S3 Stateless Loader:
```ini
CONFIG_SPIRAM_MEMTEST=n
CONFIG_SPIRAM_SPEED_40M=y
CONFIG_ESP_SYSTEM_HW_STACK_GUARD=n
CONFIG_ESP_TASK_WDT_EN=n
CONFIG_ESP_TASK_WDT_INIT=n
CONFIG_ESP_TASK_WDT_ENABLE_AT_STARTUP=n
CONFIG_PARTITION_TABLE_OFFSET=0x10000
CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384
CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE=y
CONFIG_FREERTOS_UNICORE=y
CONFIG_ESP_CONSOLE_UART=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE=0x4000
```

---

## 🔨 Building the Firmware

From `~/Desktop/seedsigner-micropython-builder`:

```bash
make docker-build-all \
  BOARD=WAVESHARE_ESP32_S3_TOUCH_LCD_35B \
  SS_EMBIT_DIR=$HOME/Desktop/seedsigner-micropython-builder/deps/embit \
  SS_APP_DIR=$HOME/Desktop/seedsigner-micropython-builder/deps/seedsigner
```

Output: `build/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/micropython.bin`

---

## 🔐 Signing & Flashing

From `Phase_16/`:

1. **Sign the bundle for SD card boot:**
   ```bash
   ./build_micropython.sh
   ```
   Generates `Phase_16/build/seedsigner_esp32s3.bin` (Specter bundle) and `Phase_16/build/micropython_esp32s3_raw.bin`.

2. **Flash dev payload (no SD card needed for bring-up):**
   ```bash
   ./flash_payload.sh
   ```
   Flashes the raw MicroPython image into the `payload` partition (`@ 0x220000`).

3. **Run the S3 Stateless Bootloader:**
   ```bash
   cd seedsigner_bootloader_esp32s3_stateless_os && ./run.sh
   ```
