# Phase 12 — SeedSigner MicroPython Builder Changes

Documentation + artifacts for the changes required in
[`seedsigner-micropython-builder`](https://github.com/SeedSigner/seedsigner-micropython-builder)
so it builds firmware that boots on top of the Phase 12 **Specter-verified stateless
bootloader** (`seedsigner_bootloader_p4_stateless_os`, sibling folder in
`SoB/Phase_12/`).

The Phase 12 loader adds Specter-DIY secp256k1 multisig verification before the
stateless PSRAM handoff (Phase 11). The MicroPython payload boots through the same
reusable `stateless_shim` component, but the shim now needs one fix to work with the
container IDF used by the builder (see Part A below).

---

## Part A — What changed from Phase 11

The only file-level difference from Phase 11 is `stateless_shim.c`, which now installs
the app's CLIC interrupt vector tables before jumping to `esp_startup_start_app()`.

**Root cause (same as Phase 12 hello-world fix, but for the builder shim):**

The shim replaces `call_start_cpu0()`, which normally calls `init_cpu()` to install
`_vector_table` / `_mtvt_table`. Without the tables, the FreeRTOS tick ISR and every
other interrupt vector through an empty/trash mtvt entry, so:
- `usb_serial_jtag_sof_tick_hook` never fires → MWDT1 (re-armed by
  `esp_int_wdt_init` inside `esp_startup_start_app`) is never fed
- → `rst:0x7 (HP_SYS_HP_WDT_RESET)` shortly after `esp_startup_start_app()`

**The fix (added to `stateless_shim.c` just before the `esp_startup_start_app` call):**

```c
extern char _vector_table[];
extern char _mtvt_table[];
esp_cpu_intr_set_ivt_addr(_vector_table);
esp_cpu_intr_set_mtvt_addr(_mtvt_table);
```

**Naming note — `mtvt` vs `xtvt`:** the container IDF (v5.5.1) uses
`esp_cpu_intr_set_mtvt_addr`; the host IDF (v5.5) also has
`esp_cpu_intr_set_xtvt_addr` as an alias. The builder shim uses the `mtvt` name
because the firmware builds inside the container. The Phase 12 hello-world shim
(`hello_world_esp32p4_stock_shim/components/stateless_shim/stateless_shim.c`) uses
the `xtvt` name because it builds on the host — both are equivalent.

---

## Files in this directory

`files/` mirrors the builder repo's `mods/new_files/` overlay at exact repo paths.
Only `stateless_shim.c` differs from the Phase 11 version; all other files are
identical (same builder, same MicroPython upstream).

| File in `files/` | Builder destination | Difference from Phase 11 |
|---|---|---|
| `.../stateless_shim/stateless_shim.c` | `deps/micropython/mods/new_files/.../stateless_shim/stateless_shim.c` | **+15 lines:** `#include "esp_cpu.h"` + CLIC vector-table install (`esp_cpu_intr_set_ivt_addr` + `esp_cpu_intr_set_mtvt_addr`) |
| all other files | (same) | Identical to Phase 11 |

---

## Build

Same as Phase 11 — the builder's MicroPython submodule stays pinned at `78ff170de`.
The overlay (with the vector-table fix) is applied by `scripts/apply_micropython_mods.sh`
at build time, so no `MP_ALLOW_DIRTY=1` is needed for the MicroPython tree:

```bash
make docker-build-all \
  BOARD=WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43 \
  SS_EMBIT_DIR=~/Desktop/seedsigner-micropython-builder/deps/embit \
  SS_APP_DIR=~/Desktop/seedsigner-micropython-builder/deps/seedsigner
```

Output: `build/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/micropython.bin`

---

## Verification

The signed bundle is verified by the Phase 12 loader (`Signature verification PASSED!`)
before the stateless shim handoff. Full serial flow:

```
Found Specter bootloader section header
Signature verification PASSED!
Image OK: 7 segments, entry=0x4FF0403A
JMP[1] entered
JMP[2] WDT and SysTick disabled
...
JMP[13] JUMP!
=== __wrap_call_start_cpu0 ENTERED ===
...
Jumping to esp_startup_start_app...
MicroPython v1.27.0-1025-g0e2d64608
Type "help()" for more information.
>>>
```

REPL input verified: `print('P12_MP_OK')` echoed; `import embit` and
`import seedsigner` succeeded (no `ImportError`).

---

## See also

- Phase 11 builder changes: `Phase_11/seedsigner_micropython_builder_changes/`
- Phase 12 debugging notes: `Phase_12/Debugging_Notes.md` (Part 5)
