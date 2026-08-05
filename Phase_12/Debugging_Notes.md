# Phase 12 — Debugging Notes: Payload never boots after `JMP[13] JUMP!`

Status: **resolved** — the Specter-signed hello-world payload and MicroPython
payload both boot end-to-end through the secure loader. Five root causes were
found and fixed:

1. [Part 1 — loader memory layout](#part-1--loader-code-overlapped-the-payload-region)
   (relocate the loader above `0x4FF40000` + evict-before-copy).
2. [Part 2 — loader stack overlapped the payload's SRAM text](#part-2--the-loaders-main-task-stack-overlapped-the-payloads-sram-text)
   (dedicated `jump_stack` + naked trampoline + HW stack-guard teardown).
3. [Part 3 — shim vector-table fix](#part-3---shim-vector-table-install-was-missing)
   (CLIC ivt/xtvt install in the hello-world shim).
4. [Part 4 — MINIMAL_BUILD drops PSRAM XIP layout](#part-4---minimal_build-silently-drops-spiram-from-the-component-set)
   (remove MINIMAL_BUILD for stock-ESP-IDF shim payloads).
5. [Part 5 — MicroPython shim: same class, different API name](#part-5--micropython-shim-clic-vector-table-fix-same-class-different-api-name)
   (mtvt naming for container IDF).
6. [Part 6 — SD card mounts but the bundle is "not found"](#part-6--sd-card-mounts-but-seedsigner_esp32p4bin-not-found-fatfs-lfn)
   (FatFs LFN disabled by default; long filename needs `CONFIG_FATFS_LFN_HEAP=y`).

## Symptom

The Phase 12 secure loader (stateless, executes payload from PSRAM via a rewired
Cache MMU) boots, maps, copies and verifies the Specter-signed payload, prints the
full `JMP[...]` sequence, then prints `JMP[13] JUMP!` — and the payload's
`K M P L` banner never appears. The very first build emitted garbage bytes
(`0x9C 0D 0A`) after the jump and then went silent; the diag builds went silent
or hung inside the cache-flush helpers.

## Environment

- Target: ESP32-P4 **rev 0** (Waveshare ESP32-P4 WiFi6 Touch LCD 4.3), ESP-IDF v5.5.
- Loader app flashed at `0x30000`; boot via normal ESP-IDF 2nd-stage bootloader
  (which itself executes from `0x4FF29ED0-0x4FF352C0`).
- Serial `/dev/ttyACM0` @115200; capture rig = esptool `--after hard_reset`.
- Payload (test): `hello_world_esp32p4_stateless_payload`, entry **`0x4FF0156C`**.
- Phase 11 reference: MicroPython payload entry **`0x4FF0403A`** boots fine.

## Root cause

1. **Address overlap.** The loader's own `.iram0.text`/`.data` live in L2MEM at
   `0x4FF00000-0x4FF247C0` (`_iram_start` = `0x4FF00000`, text to `0x4FF0FA00`,
   data `0x4FF0FA00-0x4FF12510`, bss/heap to `0x4FF247C0`). The payload is copied
   into the same region, and its entry `0x4FF0156C` sits **inside the loader's own
   text** (`esp_psram_check_ptr_addr` + `0x44`). The P4 L1 I-cache therefore still
   holds *loader* instructions on those lines when the jump happens; `fence.i`
   does not flush the P4 L1 I-cache, so the entry fetch hits stale lines.

2. **Dirty D-cache.** The direct-to-SRAM payload copies went through the
   write-back L1 D-cache, so the payload bytes were not yet in SRAM. Even a
   cold I-fetch would read stale SRAM contents.

3. **Why the ROM flush helpers can't be used.** Every L1 address-based cache op
   (`Cache_Invalidate_Addr`, `Cache_WriteBack_Invalidate_Addr`) funnels into a
   `Cache_Sync_Items`-style L2 sync engine (ROM `0x4FC104DA`) that **spins forever
   for internal-SRAM ranges** (polls `0x3FF10098` bit 16, which never clears).
   Same for `Cache_Disable_L1_DCache` (`0x4FC11782`), which chains cache-table
   calls after polling `0x3FF100D8`/`0x3FF10098`/`0x3FF10088` — it hangs too at the
   post-jump position. Only **external-memory** ranges (`0x48000000` PSRAM MMU)
   complete.

4. **Why the low address happens at all (vs Phase 11).** `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`
   in this project selects the REV_LESS_V3 memory layout: `components/esp_system/ld/esp32p4/memory.ld.in`
   defines `sram_low` at `0x4FF00000` (len `0x2CBD0`) + `sram_high` at `0x4FF40000`.
   The stock/V3 layout instead starts `sram_low` at `0x4FF00000 + CONFIG_CACHE_L2_CACHE_SIZE`
   (`0x4FF20000`). The stock layout — used by the IDF bootloader, and the reason
   Phase 11's MicroPython payload boots — **never overlaps the app IRAM region**,
   so no cache handling is required at all.

## Investigation log

| Fix | Change (after `JMP[13]`) | Result |
|---|---|---|
| — | original: `fence.i` then jump | garbage `0x9C 0D 0A`, then silence |
| fix1 | `Cache_WriteBack_Invalidate_Addr(0x13, 0x4FF00000, 0x20000)` | hang inside the call |
| fix2 | L1 I+D `Cache_Disable`/`Cache_Enable` (IDF `set_cache_and_start_app` pattern) | `JMP[13]` prints, no garbage — **not reproducible later** |
| fix3 | D-cache drain (`Cache_WriteBack_Invalidate_Addr` 0x10) | silence after `JMP[13]` |
| fix4 | dedup + earlier `0x4FF0xxxx` invalidate + `fence.i` | silence after `JMP[13]` |
| fix5 | extended diag (`CACHE[1]/[2]/[3]`, sram@entry) | hang in diag; entry still stale |
| fix6 | `Cache_Disable_L1_DCache`/`Cache_Enable_L1_DCache` + `Cache_Invalidate_All(0x33)` + `CACHE[A]/[B]` markers | hang inside `Cache_Disable_L1_DCache` — `CACHE[A]` never prints; capture ends at `JMP[13] JUMP!\r\n` |

Conclusion from fix6: even the whole-cache disable path is unusable at the jump
position. All ROM L1 ops targeting internal SRAM are dead ends.

## The fix

Relocate the loader's own code+data **off the payload's region entirely**, the way
the stock IDF layout does, instead of trying to flush a stale cache.

1. **Shift the loader's memory layout upward** via an extra linker script that
   redefines the `MEMORY` regions (GNU ld: a later `MEMORY` redefinition wins over
   the earlier one, only a "redeclaration of memory region" warning is emitted):
   - `sram_low`: org `0x4FF00000` → **`0x4FF40000`** (made RWX so `.text` fits),
     len `0x2CBD0`.
   - `sram_high`: org `0x4FF40000` → **`0x4FF6CBD0`**, len `0x53430`
     (keeps it inside SRAM top `0x4FFC0000`).

   Resulting layout (all loader segments move above the payload region):
   - payload copy region: `0x4FF00000-0x4FF20000` — loader never fetches from it → cold misses.
   - ROM data (`0x4FF3FEDC-0x4FF3FFF0`, incl. the cache table ptr at `0x4FF3FFD8`) — untouched.
   - IDF 2nd-stage bootloader region `0x4FF29ED0-0x4FF352C0` — clear of loader segments.
   - loader `.text` `0x4FF40000+`, data/bss `0x4FF4FA00+`, `evict_buf` above `0x4FF50000`.

2. **Drain the D-cache without ROM calls.** The 64KB `evict_buf` scratch buffer
   (previously at `0x4FF1304C`, dangerously overlapping the payload region) now
   sits above `0x4FF40000`, so it can be safely written **after** the payload
   copies. A write pass over it evicts the payload's dirty D-cache lines to SRAM
   (multiple passes to defeat LRU: freshly-copied lines are MRU, so one cache-size
   pass is not enough). Store the whole buffer at least 2–3× to guarantee eviction
   of every line.

3. **Remove the hanging cache block** (`Cache_Disable_L1_DCache` /
   `Cache_Enable_L1_DCache` / `Cache_Invalidate_All(0x33)` and the `CACHE[A]/[B]`
   diagnostics). Keep the external `0x48000000` invalidates (those work and are
   needed for the flash-mapped payload segments) and `fence.i`.

## Verification

Success criteria: after `JMP[13] JUMP!` the serial shows

```
K M P L
PHASE 12 ...            (payload banner)
... PSRAM hello ...     (periodic prints)
```

- Confirm with `build/seedsigner_secure_loader.map`: `sram_low` origin `0x4FF40000`,
  `_iram_start` `0x4FF40000`, `.text` `0x4FF40000-0x4FF4FA00`.
- Confirm with `python3 -m esptool image_info` (or the .elf): loader IRAM segments
  now load at `0x4FF40000+`.

## Caveats

- C1: The old pytest JMP-sequence regexes (see AGENTS.md) are still stale vs the
  current `main.c` log ordering; not part of this fix.
- C2: `Cache_Invalidate_All(0x33)` was never actually exercised (fix6 hung in the
  disable call first) — treat as untested.
- C3: Relocating the loader above the IDF bootloader region is safe only because
  the 2nd-stage bootloader loads app segments at their declared load addresses and
  its own execution window stays clear. If the loader grows past `0x4FF6CBD0`
  (its new `sram_low` end), the `sram_high` origin must be re-derived.
- C4: The fix does **not** touch the Specter verify path, MMU mapping, or the
  legacy raw-image path; it only changes where the loader's own segments sit and
  how the D-cache is drained before the jump.

---

# Part 2: The loader's main-task stack overlapped the payload's SRAM text

Follow-on bug. After Part 1, the loader relocated cleanly and copied the payload,
but the payload still panic'ed *after* `--- ENTERED my_entry_point! ---` and
`Attempting to jump to call_start_cpu0...`.

## Symptom (two distinct panics, same session)

1. **Instruction access fault** — `Guru Meditation Error: Core 0 panic'ed
   (Instruction access fault)`, `MEPC=0x4ff0462c`, `SP=0x4ff04590`,
   `MTVAL=0x000001c8`. `0x4ff0462c` is the first instruction of
   `rtc_clk_cpu_freq_get_config` (`esp_hw_support/port/esp32p4/rtc_clk.c:453`).
   The stack dump at `0x4ff04630-0x4ff04700` contained the loader's own
   `do_mmu_mapping_and_jump()` stack locals — the `JMP[8] ... paddr= ... pages=
   ...  entry= ... val= ... OK` format strings and `JMP[6] D-cache drained
   post-copy` — interleaved with the payload's `rtc_clk.c` code.
2. **Stack protection fault** — after moving the stack, `Stack protection fault,
   Detected in task "main" at 0x4000ba98`, `Stack pointer: 0x4ff5aed0`,
   `Stack bounds: 0x4ff00934 - 0x4ff04b30`. `0x4000ba98` is the trampoline's
   `mv sp, ...`.

## Root cause

The loader's FreeRTOS **main-task stack was carved from the low RETENT_RAM heap
region** (`heap_init: At 4FF00000 len 0003AFC0`) and landed at
`0x4ff04590` — *inside* the payload's internal-SRAM `.text`
(`.iram0.text 0x4ff00000-0x4ff0e942`). Consequences:

1. **JMP-zone clobbering.** `do_mmu_mapping_and_jump()` runs on that stack. Its
   stack-local string arrays live at `0x4ff045c0-0x4ff04700`. The payload SRAM
   copy (`JMP[4]`) writes the image there, but the *subsequent* `JMP[8]` MMU-print
   loop and `JMP[6]` drain re-write those same cache lines with the loader's
   format strings. The drain bakes the strings into SRAM at `0x4ff0462c+`,
   clobbering `rtc_clk_cpu_freq_get_config`. The payload boots from PSRAM, calls
   it, fetches garbage → instruction access fault.
2. **Inherited SP.** The payload's `my_entry_point` (this build) did not set its
   own stack, so it kept running on `0x4ff04590` — inside its own `.text` — for
   the whole `call_start_cpu0` early-boot path.
3. **HW stack guard.** The ESP32-P4 `assist_debug` peripheral monitors the running
   task's SP against bounds set by FreeRTOS (`0x4ff00934-0x4ff04b30` for the
   "main" task). Any SP switch out of those bounds → `Stack protection fault`.

Why Phase 11 MicroPython survived all three: its SRAM `.text` is tiny (ends before
`0x4ff04590`), so the JMP-zone strings never clobbered it, and its shim entry sets
its own `custom_boot_stack` in `.data` immediately.

## The fix

1. **Dedicated jump stack.** `static uint8_t jump_stack[32768]` (in the relocated
   loader `.bss`, `0x4ff53050-0x4ff5b050`, clear of the payload region). A naked
   trampoline `do_mmu_mapping_and_jump_trampoline()` is called instead of
   `do_mmu_mapping_and_jump()`; it switches `sp` to `jump_stack` top *before* any
   payload copy, so the whole JMP zone and the payload's inherited early-boot SP
   are safely above the payload area.
2. **Stack-guard teardown in the trampoline** (before `mv sp`): `csrw mie, zero`
   (no tick → the scheduler can't re-arm the monitor with the next task's
   bounds), widen `assist_debug` `SP_MAX_REG` to `0xFFFFFFFF` / `SP_MIN_REG` to
   `0`, clear the latched spill (`INTR_CLR_REG`), and clear the SP-spill `ENA`
   bits (`INTR_ENA_REG`, base `0x3FF06000`).
3. `do_mmu_mapping_and_jump()` made non-static so the trampoline's `call` can bind
   to it (asm references the global symbol).

## Verification (final)

```
Signature verification PASSED!
Image OK: 6 segments, entry=0x480097B4
Jumping to 0x480097B4 ...
JMP[1] entered ... JMP[13] JUMP!
JMP[6] D-cache drained post-copy
--- ENTERED my_entry_point! ---
Attempting to jump to call_start_cpu0...
[Intercepted] cache_hal_init() - preserving cache! ...
PHASE 12: STATELESS PAYLOAD (call_start_cpu0 path)
Hello from a FreeRTOS Task running completely in PSRAM!   (every ~1 s, stable)
```

Single `rst:0x1 (POWERON)`; 23+ periodic hellos with no further reset.

## Part 2 caveats

- C5: The loader's main-task stack still physically overlaps the payload region;
  it is simply abandoned (never written) from the trampoline on, and the `JMP[4]`
  copy overwrites its contents. If a future payload's SRAM footprint grows past
  `0x4ff20000` the low heap region must be excluded from the loader's allocator
  too.
- C6: The payload still inherits the loader's SP. A payload-side
  `custom_boot_stack` (as MicroPython does) remains a good defense-in-depth, but
  is not required now that the loader hands over a safe stack.

---

## Part 3 — `stateless_shim` boot path (WDT reset → vector-table fix)

The whole point of the shim is that it is **reusable**: the same
`components/stateless_shim` the Phase 11 MicroPython build uses should boot any
plain ESP-IDF app. So the `call_start_cpu0` + `--wrap`-interceptor payload from
Part 2 was replaced with the shim (restored verbatim from commit `64a9cf7`).

### Symptom

The shim hand-off ran almost all the way:

```
=== __wrap_call_start_cpu0 ENTERED ===
BSS cleared. / Clocks initialized. / Initializing MMU software contexts...
Calling init functions...   (all stage-0 fns: cpu_start, app_init, heap_init, ...)
Running global constructors...
Manually adding PSRAM to heap...
Jumping to esp_startup_start_app...
rst:0x7 (HP_SYS_HP_WDT_RESET), Core0 Saved PC:0x4ff01440     <- WDT kills us here
```

`rst:0x7` = `HP_SYS_HP_WDT_RESET` = `RESET_REASON_CORE_MWDT` (main watchdog).
The loader's `JMP[2]` already disables SWD/LP_WDT/MWDT0/MWDT1/RWDT, so the reset
had to come from something the **payload** re-armed.

### Root cause

The saved PC `0x4ff01440` disassembles to the **middle of
`usb_serial_jtag_sof_tick_hook`** (`lui a5,0x4ff13; lbu a5,1996(a5)` →
`s_usb_serial_jtag_conn_status` @ `0x4ff137cc`) — a FreeRTOS tick hook in the
payload. So the tick *was* firing, but the app still got MWDT-reset shortly after
`esp_startup_start_app()`.

In a normal IDF boot, `call_start_cpu0()` → `init_cpu()` installs the app's CLIC
vector tables (`_vector_table` @ `0x4ff00000`, `_mtvt_table` @ `0x4ff00040`). The
shim *replaces* `call_start_cpu0()` and never did that. `esp_startup_start_app()`
then runs `esp_int_wdt_init()` (see
`$IDF/components/esp_system/int_wdt.c`), which on the P4 arms **MWDT1** (TIMG1,
the IWDT) with `IWDT_INITIAL_TIMEOUT_S 5` and feeds it only from a registered
FreeRTOS tick hook. With the app's vector tables missing, that feed never ran
properly → MWDT1 reset. MicroPython's shim survived only because its own
boot path sets up interrupt routing elsewhere.

### The fix

In `stateless_shim.c`, right before jumping into `esp_startup_start_app()`,
install the app's own vector tables (the exact `init_cpu()` calls):

```c
extern char _vector_table[];
extern char _mtvt_table[];
esp_cpu_intr_set_ivt_addr(_vector_table);   // rv_utils_set_xtvec -> mtvec
esp_cpu_intr_set_xtvt_addr(_mtvt_table);    // CLIC xtvt (mtvt in M-mode)
```

### Verification

Full boot through the shim, stable:

```
JMP[13] JUMP! / JMP[6] D-cache drained post-copy
=== __wrap_call_start_cpu0 ENTERED ===
... init-fn array ... Manually adding PSRAM to heap...
Jumping to esp_startup_start_app...
I (6386) main_task: Started on CPU0
I (6386) main_task: Calling app_main()
PHASE 12: SHIM-BASED STATELESS PAYLOAD
Hello from a FreeRTOS Task running completely in PSRAM (shim boot)!  (every ~1 s)
```

21+ periodic hellos with zero `HP_SYS_HP_WDT_RESET`.

### Part 3 caveats

- C7: This makes the shim boot *any* plain ESP-IDF app. The Phase 11 MicroPython
  shim does not have the vector-table install; folding it in there is harmless
  (its own interrupt setup would simply override) but should be re-tested before
  being treated as verified.
- C8: The `my_trap_handler` the shim installs early (mtvec) is overwritten by the
  vector-table install, so it only covers the pre-`esp_startup_start_app` window.

---

## Part 4 — Stock ESP-IDF `hello_world` through the shim (MINIMAL_BUILD gotcha)

To prove the shim is genuinely drop-in, the **unmodified**
`$IDF/examples/get-started/hello_world` was copied to
`hello_world_esp32p4_stock_shim/` (only `main/CMakeLists.txt` gained
`REQUIRES stateless_shim`, `sdkconfig.defaults` was copied from the working
payload, and the example's `MINIMAL_BUILD` line was removed). `hello_world_main.c`
was never touched.

### Symptom

`JMP[3]` cache eviction faulted:

```
JMP[1] entered
JMP[2] WDT and SysTick disabled
JMP[3] interrupts off, starting cache eviction
JMP[5] D-cache evicted
Guru Meditation Error: Core  0 panic'ed (Store access fault). Exception was unhandled.
MEPC    : 0x50108416   MTVAL  : 0x40010020   MCAUSE : 0x00000007
```

…then `Rebooting...` → `rst:0xc (SW_CPU_RESET)` → infinite loader re-verify loop.
No payload output at all.

### Root cause

The loader MMU-maps payload segments with load addresses in
`0x48000000–0x4C000000` to PSRAM, and **direct-copies** every other segment.
On the P4, `0x40000000–0x44000000` is the flash-cache IROM/DROM window
(read-only), so direct-copying the payload's `0x40000000` segments faults on the
first store.

The stock example sets `idf_build_set_property(MINIMAL_BUILD ON)`, which restricts
the component set to `main` + transitive `REQUIRES`. `esp_psram` (which owns the
`CONFIG_SPIRAM_*` Kconfig symbols) is not pulled in, so the PSRAM XIP options from
`hello_world_esp32p4_stateless_payload/sdkconfig.defaults` were **silently
dropped** — the generated `sdkconfig` had zero `CONFIG_SPIRAM` entries. Without
`CONFIG_SPIRAM_FETCH_INSTRUCTIONS/RODATA/XIP_FROM_PSRAM`, the P4 linker script
places the app's IROM/DROM in the `0x40000000` flash window instead of
`0x48000000`.

### The fix

Remove the `idf_build_set_property(MINIMAL_BUILD ON)` line from the top-level
`CMakeLists.txt`. Reconfigure, and the full component set restores
`CONFIG_SPIRAM=y` + XIP options; the ELF's IROM LOAD segment moves from
`0x40000020` to `0x48000020`.

### Verification

```
Signature verification PASSED!  →  JMP[13] JUMP!
=== __wrap_call_start_cpu0 ENTERED ===
... init-fn array ... Manually adding PSRAM to heap...
Jumping to esp_startup_start_app...
Hello world!
This is esp32p4 chip with 2 CPU core(s), , silicon revision v1.3, 8MB external flash
Minimum free heap size: 25752152 bytes
Restarting in 10 seconds...   (countdown to 0)
Restarting now.   →   rst:0xc (SW_CPU_RESET)   →   full second boot cycle
```

2 hellos across two full `esp_restart()` cycles, zero `HP_SYS_HP_WDT_RESET`, zero
panics.

## Part 5 — MicroPython shim: CLIC vector-table fix (same class, different API name)

### Symptom

After applying the Part 3 vector-table fix to the **hello-world shim** (host IDF), the
Phase 12 MicroPython build still died with `rst:0x7 (HP_SYS_HP_WDT_RESET)`. The
boot flow reached `Jumping to esp_startup_start_app...` then immediately watchdog-reset.
No `MicroPython` banner, no REPL.

### Root cause

The MicroPython shim in the builder repo had **no** CLIC vector-table install — same
class as Part 3, just never applied to the builder's `stateless_shim.c`. Without the
tables, `esp_startup_start_app()` arms MWDT1, the tick ISR is unreachable via mtvt,
and the watchdog fires.

### API naming difference

The builder builds MicroPython inside a Docker container running IDF v5.5.1, which
exposes `esp_cpu_intr_set_mtvt_addr()`. The host IDF (v5.5) exposes both
`esp_cpu_intr_set_xtvt_addr()` and `esp_cpu_intr_set_mtvt_addr()`. The builder shim
**must** use the `mtvt` name; the Phase 12 hello-world shim (built on the host) uses
the `xtvt` alias. Both are equivalent on the host.

### The fix

Applied to `~/Desktop/seedsigner-micropython-builder/deps/micropython/mods/new_files/
ports/esp32/boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/stateless_shim/stateless_shim.c`,
mirrored to `Phase_12/seedsigner_micropython_builder_changes/files/...`:

```c
#include "esp_cpu.h"

// ... inside __wrap_call_start_cpu0, just before esp_startup_start_app():

    extern char _vector_table[];
    extern char _mtvt_table[];
    esp_cpu_intr_set_ivt_addr(_vector_table);
    esp_cpu_intr_set_mtvt_addr(_mtvt_table);
```

After rebuild and re-sign: `Signature verification PASSED!` → shim →
`MicroPython v1.27.0-1...` → REPL `>>>`. Zero `HP_SYS_HP_WDT_RESET`.

### Verification

```
Found Specter bootloader section header
Signature verification PASSED!
Image OK: 7 segments, entry=0x4FF0403A
JMP[1] entered
...
JMP[13] JUMP!
=== __wrap_call_start_cpu0 ENTERED ===
...
Jumping to esp_startup_start_app...
MicroPython v1.27.0-1025-g0e2d64608
Type "help()" for more information.
>>> print('P12_MP_OK')
P12_MP_OK
>>> import embit
>>> import seedsigner
>>>
```

## Part 6 — SD card mounts but `seedsigner_esp32p4.bin` "not found" (FatFs LFN)

### Symptom

The Phase 12 loader mounts the SD card successfully (`SD card mounted at /sdcard`)
but then halts with `Firmware file /sdcard/seedsigner_esp32p4.bin not found` — even
though the file is present at the card root. Earlier SD-card payload reads (Phase 9
`seed.bin`, Phase 10 `hello.txt`) worked fine, so this looked baffling at first.

### Root cause

ESP-IDF FatFs defaults to **`CONFIG_FATFS_LFN_NONE`** — long-filename support is
compiled out, so FatFs only matches 8.3 short names (`SEEDSIG~1.BIN`). The new
bundle filename `seedsigner_esp32p4.bin` is **22 chars**, far beyond 8.3, so
`stat()`/`fopen()` fail with "not found" despite a perfectly good mount and
directory. The earlier payloads worked because their names (`seed.bin`, `hello.txt`)
fit in 8.3.

### The fix

Enable long filenames in `sdkconfig.defaults`:

```
CONFIG_FATFS_LFN_HEAP=y
```

(HEAP, not STACK — the LFN buffer is 256 bytes, don't grow the loader's stack for
it.) The value must be set *before* the first build; flipping it later needs a
clean re-run (`idf.py fullclean` or delete the generated `sdkconfig`), because the
generated `sdkconfig` pins `CONFIG_FATFS_LFN_NONE=y`.

### Verification

After enabling LFN the loader reads the file: `SD card mounted at /sdcard` →
`[SD CARD] Loaded N bytes from /sdcard/seedsigner_esp32p4.bin` → verification →
`JMP[1]`…`JMP[8]` → payload.
