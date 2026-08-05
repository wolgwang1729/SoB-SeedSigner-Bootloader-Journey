# Phase 11 — SeedSigner MicroPython Builder Changes

Documentation + artifacts for the changes required in
[`seedsigner-micropython-builder`](https://github.com/SeedSigner/seedsigner-micropython-builder)
so it builds firmware that boots on top of the Phase 11 **stateless bootloader**
(`seedsigner_bootloader_p4_stateless_os`, sibling folder in `SoB/Phase_11/`).

The custom bootloader flashes the app to `0x10000`, jumps straight into the app's
custom entry point, and **never runs the ESP-IDF 2nd-stage bootloader**. That means
the firmware itself must re-implement the ESP-IDF early-boot hand-off (entry point,
`.bss` clearing, clock init, PSRAM heap injection) that the normal bootloader would
otherwise do. All of that lives in the **stateless shim** described below.

Target board: `WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43` (ESP32-P4, 16 MB flash,
no display connected in this dev setup).

---

## How the pieces fit together

```
stateless bootloader (flashes app @0x10000)
        │  jumps to my_entry_point (the app's linker entry, -e my_entry_point)
        ▼
stateless_shim.c  (new ESP-IDF component inside the app)
        │  naked asm entry: enable FPU, set gp/sp, tail-jump to __wrap_call_start_cpu0
        │  __wrap_call_start_cpu0:
        │     disable RWDT/SWD/LP_WDT/MWDT0/MWDT1 watchdogs
        │     memset all .bss regions
        │     esp_clk_init / esp_perip_clk_init / clear CLIC interrupt routing
        │     esp_mmu_map_init (software MMU context — needed for flash/partitions)
        │     run _esp_system_init_fn_array stage 0 → global ctors → stage 1
        │     heap_caps_add_region_with_caps(PSRAM 0x48800000-0x4A000000)
        │  tail-jump esp_startup_start_app (native FreeRTOS scheduler boot)
        ▼
MicroPython app_main() → board_init() → display_manager → REPL
```

The shim uses linker `--wrap` intercepts so destructive ESP-IDF routines that would
re-initialize hardware the bootloader already set up (PSRAM chip, flash tuning, cache)
become no-ops, while the ones that only maintain **software** state
(`esp_mmu_map_init`, `spi_flash_init_chip_state`, `esp_mspi_pin_*`) call through to the
real implementations.

---

## Repository state (as of this writing)

Main repo (`seedsigner-micropython-builder`) HEAD: `8b33304`
(`chore(deps): point board_common submodule at the stateless-boot fork pin`), on top of `56b9a86`
(`fix(build): guard machine_wdt for WDT-disabled boards`), `02bdf7c`
(`fix(display_manager): boot headless when no LCD is connected`) and `e988eb1`
(`feat(esp32p4): add stateless boot shim and board config for custom bootloader`).

These 4 commits sit on top of upstream `f487816` and are pushed to
`https://github.com/wolgwang1729/seedsigner-micropython-builder` (branch `main`).

| Submodule | Pin | Notes |
|---|---|---|
| `ports/esp32/board_common` | `151c6d8` (Phase 11 commits `5182161` + `151c6d8` on top of `4fd8c5a`) | ST7701/GT911 no-screen skip + NULL-panel LVGL guards. **Not on upstream** — the builder's `.gitmodules` points this submodule at `https://github.com/wolgwang1729/esp-board-common.git` where `151c6d8` lives on the `stateless-boot` branch |
| `deps/micropython/upstream` | `78ff170de` (**stock** v1.27.0 — unchanged from the base repo) | no micropython commits needed; all changes travel via the patch + overlay below |

The MicroPython submodule is **pinned at the clean upstream base** (`78ff170de`), so a
clean `make docker-build-all` works from scratch with no local micropython commits and
no `MP_ALLOW_DIRTY=1`. Everything else is carried by the repo itself:
`deps/micropython/mods/patches/0001-esp32-integration-mods.patch` (regenerated to include
the `main.c` + `uart.c` bootloader edits) plus the `mods/new_files/` overlay.

`deps/seedsigner` and `deps/seedsigner-lvgl-screens` are left at their stock pins;
their local "new commits" dirtiness is unrelated to the bootloader work and is **not**
part of these 4 commits (and does not block the build — only the MicroPython tree
is checked for cleanliness).

---

## Changes, file by file

All artifacts are mirrored under `files/` at their exact repo destination paths.

### 1. stateless_shim component — NEW

**Destination:**
`deps/micropython/mods/new_files/ports/esp32/boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/stateless_shim/`
(→ copied into the upstream submodule at build time by `scripts/apply_micropython_mods.sh`)

- `CMakeLists.txt` — registers the component and all `--wrap` linker flags, forces the
  custom entry point: `-Wl,-e,my_entry_point`, `-u __real_call_start_cpu0`,
  `-u __wrap_call_start_cpu0`.
- `stateless_shim.c` — the boot shim itself (the authoritative current version, with
  `IRAM_ATTR` on all shim routines and the REPL-RX wiring; see caveat §C1).
- `stateless_shim.c` also implements `__wrap_uart_stdout_init()`. MicroPython's stock
  `uart_stdout_init()` (which installs the UART0 RX interrupt feeding the REPL) is
  link-wrapped so the bootloader-configured UART is **not reset** (`uart_hal_init()`).
  The wrap instead wires up just the RX path: `esp_intr_alloc` for `uart_irq_handler`
  + RX-FIFO-full/timeout thresholds + RX interrupt enable. Without this, TX works but
  the REPL never receives keystrokes (the cursor sits at `>>>` and nothing echoes).

### 1b. upstream `ports/esp32/uart.c` — modify (captured in the regenerated `0001` patch)

**Destination:** `deps/micropython/upstream/ports/esp32/uart.c` (mirrored at
`files/deps/micropython/upstream/ports/esp32/uart.c`; the hunk also ships in
`files/deps/micropython/mods/patches/0001-esp32-integration-mods.patch`)

Two one-line changes so the shim can register the REPL ISR:
- forward declaration (was `static`): `void uart_irq_handler(void *arg);`
- definition (was `static`): `void IRAM_ATTR uart_irq_handler(void *arg)`

This now lives inside `0001-esp32-integration-mods.patch` (the patch was regenerated to
fold in the bootloader hand-edits; see §8), so it survives a fully clean rebuild.

### 2. mpconfigboard.cmake — modify

**Destination:**
`deps/micropython/mods/new_files/ports/esp32/boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/mpconfigboard.cmake`

Append (so the shim component gets compiled into the firmware):
```cmake
list(APPEND MICROPY_EXTRA_COMPONENT_DIRS "${MICROPY_BOARD_DIR}/stateless_shim")
```

### 3. mpconfigboard.h — modify

**Destination:**
`deps/micropython/mods/new_files/ports/esp32/boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/mpconfigboard.h`

Append (the stateless shim never starts CPU1; the MicroPython task and all
FreeRTOS-thread spawns must run on CPU0):
```c
// Run on CPU0 since CPU1 is never started by my stateless shim
#define MP_TASK_COREID                      (0)
```

### 4. sdkconfig.board — modify

**Destination:**
`deps/micropython/mods/new_files/ports/esp32/boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/sdkconfig.board`

Append the **Phase 10 hand-off configs** (match the stateless loader's expectations):
```
CONFIG_SPIRAM_MEMTEST=n
CONFIG_ESP_SYSTEM_HW_STACK_GUARD=n
CONFIG_ESP_TASK_WDT_EN=n
CONFIG_ESP_TASK_WDT_INIT=n
CONFIG_ESP_TASK_WDT_ENABLE_AT_STARTUP=n
CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n
CONFIG_PARTITION_TABLE_OFFSET=0x20000
CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384
CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE=y
CONFIG_FREERTOS_UNICORE=y
CONFIG_ESP_CONSOLE_UART=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

### 5. partitions-32MiB-waveshare.csv — modify

**Destination:**
`deps/micropython/mods/new_files/ports/esp32/partitions-32MiB-waveshare.csv`

Shifted to accommodate the stateless loader's flash layout
(`CONFIG_PARTITION_TABLE_OFFSET=0x20000`; the bootloader owns `0x0-0x1FFFF`):
- `nvs` and `phy_init` offsets are now auto-placed (blank column) instead of `0x9000`/`0xf000`
- `factory` moved `0x10000` → `0x140000`

### 6. display_manager.cpp — modify

**Destination:** `ports/esp32/display_manager/display_manager.cpp`

One Phase 11 addition (in builder commit `02bdf7c`):
- **Headless tolerance** — when no LCD is connected, `lvgl_port_setup()` creates no
  display, so `set_display()`/`overlay_manager_init()` are skipped and the board boots
  straight to the MicroPython REPL.

(The earlier cross-boot stage-tracking diagnostic — `p11_stage(n)` writes to raw LP RAM
at `0x50109000`, next boot prints `[P11-recover] prev_boot_stage=N` — was removed in the
cleanup. It was post-mortem debugging, not required for the bootloader to work.)

### 7. board_common submodule — pin + modify

**Destination:** `ports/esp32/board_common` (git submodule)

Pin the submodule to commit `151c6d8` (on the `stateless-boot` branch, rebased onto
upstream `4fd8c5a` so the board_pipeline camera API is present). It carries two commits:

- `5182161` — skip ST7701 MIPI-DSI display init and GT911 touch init when no screen is
  connected (board now logs `Skipping ST7701 MIPI-DSI display init (no screen
  connected)` and NULLs the panel/touch handles); NULL-guards the ST7701 rotate/blit
  paths so they can't dereference a NULL panel.
- `151c6d8` — **the critical fix**: in `lvgl_port_setup()`, the ST7701 landscape branch
  must NOT build the LVGL display + flush pipeline when `panel_handle == NULL`. Building
  it for a non-existent panel caused the whole CPU to wedge on the first LVGL refresh
  (~7340 ms into boot, right after `Board initialized (landscape=1).`) — no Guru
  Meditation, no reboot.

The diff for both commits is in the board_common repo itself (`git log 4fd8c5a..151c6d8`).

### 8. upstream `0001-esp32-integration-mods.patch` — regenerated to include the bootloader hand-edits

**Destination:** `deps/micropython/mods/patches/0001-esp32-integration-mods.patch`
(mirrored at `files/deps/micropython/mods/patches/0001-esp32-integration-mods.patch`)

The integration patch was **regenerated** (`scripts/generate_micropython_patch.sh`) so a
clean rebuild applies everything — no local MicroPython submodule commits required. On
top of the stock builder changes (CMakeLists, `sdkconfig.base`, `esp32_common.cmake`,
`dependencies.lock.esp32p4`, `machine_sdcard.c`, `main/CMakeLists.txt`,
`main/idf_component.yml`) it now also carries:

- **`ports/esp32/main.c`** — the `boardctrl_startup()` hand-edits:
  - NVS flash init commented out (`nvs_flash_init()`).
  - Flash size hardcoded: `esp_flash_default_chip->size = 16384 * 1024;` instead of
    `esp_flash_get_physical_size()`.
  - The auto-create of a "vfs" FAT partition is commented out.
- **`ports/esp32/uart.c`** — the two `uart_irq_handler` export lines from §1b.

(These exist because the stateless loader's flash layout doesn't match what the normal
ESP-IDF flash/VFS init expects.)

Note on `machine_wdt.c`: the board config disables the task WDT
(`CONFIG_ESP_TASK_WDT_EN=n`), and in ESP-IDF 5.5.1 the task-WDT implementation is
compiled **conditionally** — `esp_system/CMakeLists.txt` only adds
`task_wdt/task_wdt.c` to the build when `CONFIG_ESP_TASK_WDT_EN` is set. The header
declares `esp_task_wdt_*` unconditionally, but with the option off those symbols are
**not linked**, so the stock file fails at link time with `undefined reference to
esp_task_wdt_reconfigure / esp_task_wdt_add_user / esp_task_wdt_reset_user`. Fix:
`deps/micropython/mods/new_files/ports/esp32/machine_wdt.c` (mirrored at
`files/deps/micropython/mods/new_files/ports/esp32/machine_wdt.c`) wraps the
`esp_task_wdt_*` calls in `#if CONFIG_ESP_TASK_WDT_EN`, making `machine.WDT` a safe
no-op in WDT-less builds. It ships via the `new_files/` overlay (the patch generator
excludes overlay paths, so the patch needs no machine_wdt.c hunk).

---

## Build

From the builder root. The MicroPython submodule is at the clean pinned base, so a
plain build works — no `MP_ALLOW_DIRTY=1`:

```bash
rm -rf build
make docker-build-all \
  BOARD=WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43 \
  SS_EMBIT_DIR=$HOME/Desktop/seedsigner-micropython-builder/deps/embit \
  SS_APP_DIR=$HOME/Desktop/seedsigner-micropython-builder/deps/seedsigner
```

Output: `build/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/micropython.bin` (+ `.elf`,
bootloader, partition table, `flash_args`).

## Flash

`micropython.bin` is the **app** image; it is flashed by the stateless loader (it is not
a self-contained esptool image). See the loader's `run_micropython.sh` / `run.sh` in
`SoB/Phase_11/`.

## Verification

A healthy no-display boot ends in the MicroPython REPL **with working input** (the
UART0 RX interrupt from §1/§1b is active — keystrokes echo and execute, e.g.
`5+6` → `11`):

```
I (....) board: Initializing Waveshare ESP32-P4 WiFi6 Touch LCD 4.3...
I (....) display_st7701: Skipping ST7701 MIPI-DSI display init (no screen connected)
I (....) board: Skipping GT911 Touch init (no screen/touch connected)
I (....) LVGL: Starting LVGL task
I (....) board: Board initialized (landscape=1).
I (....) display_manager: No display connected — skipping set_display/overlay_manager_init
I (....) main_task: MicroPython v1.27.0 ...
>>> 5+6
11
>>>
```

This end-to-end state (stateless boot → headless board init → REPL with input) has been
**verified on hardware** (Waveshare P4, no LCD connected).

Note: the earlier `[P11-recover] prev_boot_stage=N` line was part of the temporary
stage-tracking diagnostic and is gone now that it's been stripped from
`display_manager.cpp`.

---

## Caveats & gotchas

- **C1 — overlay shim is current.** `mods/new_files/.../stateless_shim.c` (and the
  `files/` copy) match the shim that was hardware-verified. A clean rebuild applies it
  via the overlay — no submodule commit involved anymore.
- **C2 — clean-build tree == verified tree.** The regenerated `0001` patch (§8) plus the
  overlay reproduce the exact tree of the last-known-good firmware build, verified by
  diffing a simulated clean build (base `78ff170de` + patch + overlay) against it — the
  only differences are a cosmetic comment line in `sdkconfig.board` ("32MB flash" vs
  "16MB flash") and the re-added `machine_wdt.c` WDT guard (§8), which matches the
  original hardware-verified firmware. A clean `make docker-build-all` with the final
  commit set has since been run and **passed** (applies → compiles → links).
- **C3 — no display in this setup.** The ST7701/GT911 skip + NULL-panel guards (§7)
  make the firmware boot headless. On a board WITH a display, `panel_handle != NULL`
  and the full LVGL pipeline is built as before.
- **C4 — REPL input.** With the §1/§1b fix, the REPL accepts keystrokes again. If it
  ever doesn't, the first suspect is `esp_intr_alloc` failing (the shim logs
  `[P11] uart_stdout_init: esp_intr_alloc failed <n>`) — that would mean CLIC interrupt
  routing isn't up yet at `main()` time, which would also break the FreeRTOS tick.
