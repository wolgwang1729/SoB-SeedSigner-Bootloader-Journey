# Phase 11: MicroPython on App Loader

- **Date:** July 29, 2026
- **Author:** Mayank (wolgwang)
- **Project:** SeedSigner Secure Boot — Summer of Bitcoin

## 1. Previous Attempts (Phase 10)
During Phase 10, the following steps and challenges were encountered while attempting this as a stretch goal:

1. **Compilation & Shim:** MicroPython was built using the SeedSigner builder for the `WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43`. The `stateless_shim` was integrated to intercept hardware resets (`cache_hal_init`, `esp_mmu_map_init`, `esp_psram_init`, etc.) and prevent breaking the PSRAM MMU context.
2. **Firmware Size Adjustment:** The bootloader's `MAX_FIRMWARE_SIZE` was increased to 8 MB to fit the ~4.4 MB MicroPython binary.
3. **The `.bss` Overwrite Hazard:** It was discovered that MicroPython's enormous `.bss` segment spans up to `0x4FF6118C`, which threatened to completely overwrite the default bootloader stack during `call_start_cpu0`'s `init_bss` step.
4. **Stack Relocation & L2 Cache Trap:** To avoid `.bss` overwrite, the stack pointer (`sp`) in `my_entry_point` was relocated. Initial placement at `0x4FFBFFF0` crashed because it fell inside the top 128KB of HP SRAM (configured as L2 Cache by the ROM bootloader). It was successfully relocated to `0x4FF9FFF0`, safely below the L2 Cache and above MicroPython's `.bss`.
5. **Stalled Progress:** Despite fixing the stack and memory layout issues, the CPU still hung silently immediately upon entering `call_start_cpu0`. Standard UART debugging output failed to flush, likely due to double faults, watchdog resets during the massive `.bss` zeroing, or violated architectural state assumptions. The effort stalled here and has now been moved to this phase.

## 2. Overview

Phase 10 ended with the stateless PSRAM bootloader (`seedsigner_bootloader_p4_stateless_os`) successfully executing stock ESP-IDF apps and FreeRTOS tasks from PSRAM, but **stalled** on the stretch goal of booting MicroPython: the CPU hung silently right after entering `call_start_cpu0`. Phase 11 picks up exactly there and carries the stretch goal across the finish line.

To bridge the gap, this phase re-implemented the ESP-IDF early-boot hand-off that the normal 2nd-stage bootloader would otherwise perform — because the stateless loader flashes the app at `0x10000`, jumps straight to its custom entry point, and **never runs the ESP-IDF 2nd-stage bootloader**. The firmware itself had to reconstruct the entry hand-off via a `stateless_shim` component: FPU enable, `.bss` clearing, clock/MMU init, watchdog disarm, and PSRAM heap injection — all while link-`--wrap`ing the ESP-IDF routines that would reset hardware the bootloader already configured.

The debugging below (Section "The Silent Heap Hang") documents the process brick by brick, from the first silent `heap_caps_init` hang through to a hardware-verified REPL with working keyboard input.

## 3. Target

1. Take the Phase 10 `seedsigner_bootloader_p4_stateless_os` + MicroPython builder setup as the starting point.
2. Identify why the CPU hangs silently inside ESP-IDF's early boot when entering `call_start_cpu0` under the stateless loader, and fix it.
3. Rebuild the full ESP-IDF early-boot hand-off in a `stateless_shim` so MicroPython boots cleanly on top of the custom app loader (entry point, `.bss`, clocks, MMU, PSRAM heap, watchdogs).
4. Get the board to boot headless into the MicroPython REPL (no LCD connected in this dev setup) with **working UART input**, not just output.
5. Make the whole thing reproducible from a clean checkout: no local MicroPython submodule commits, a regenerated `0001-esp32-integration-mods.patch` + overlay, and a passing clean `make docker-build-all`.


## 4. The Silent Heap Hang: Debugging Summary

This document summarizes the debugging saga to get the SeedSigner stateless PSRAM bootloader working with MicroPython on the ESP32-P4.

### The Core Problem
The firmware was suffering from a "silent hang" during early boot. The CPU would completely freeze without throwing any exceptions, panic messages, or rebooting. This occurred right after the initial application banner was printed but before the RTOS scheduler started.

### Debugging Workflow & Discoveries

#### 1. Intercepting the Boot Sequence (`stateless_shim.c`)
Because the custom bootloader already sets up the MMU, Cache, Flash, and SPI states, allowing ESP-IDF's `call_start_cpu0` to run normally caused immediate crashes (as IDF would try to re-initialize and clobber the working hardware state).
*   **Fix**: I used linker `--wrap` flags to intercept `call_start_cpu0` and dozens of hardware initialization functions (like `cache_hal_init`, `esp_mmu_map_init`, etc.) to preserve the bootloader's state.

#### 2. Line-by-Line Execution Tracing
To find the exact location of the silent hang in the ESP-IDF startup routines, I implemented a custom runner for the `ESP_SYSTEM_INIT_FN` array in `stateless_shim.c`.
*   I used `esp_rom_printf` and a UART FIFO drain loop to print the function pointer of every initialization routine *before* and *after* executing it.
*   **Result**: This isolated the hang to `0x480ad59e`, which corresponded to `heap_caps_init()` (the early heap initialization phase).

#### 3. Isolating `multi_heap_register`
Inside `heap_caps_init`, the system loops over available SRAM regions and registers them using `multi_heap_register`. I intercepted this specific function to trace its inputs.
*   The hang consistently occurred when registering the first HP SRAM region (`0x4ff20d90` with size ~104 KiB).

#### 4. Proving SRAM Power State (Dense Memset Test)
I suspected an AXI/AHB bus freeze. On ESP32 chips, if a memory domain is unpowered (which was possible since I intercepted the PMU initialization `esp_rtc_init`), reading or writing to it freezes the CPU bus without any software exception.
*   **Fix**: I wrote a "dense memory probe" directly into my wrapper, writing `0xDEADxxxx` to *every single 32-bit word* in the 104KB region. 
*   **Result**: The test passed perfectly. The SRAM was fully powered and writable, definitively ruling out a hardware bus freeze.

#### 5. The TLSF Alignment & Panic Deadlock
By inspecting the ESP-IDF heap implementation (`multi_heap_register_impl` and `tlsf.c`), I found a lethal combination of bugs:
1.  **Alignment**: TLSF requires 8-byte aligned memory. IDF reserves 20 bytes (`sizeof(heap_t)`) at the start of the heap. `0x4ff20d90 + 20 = 0x4ff20da4`, which is NOT 8-byte aligned.
2.  **The Deadlock**: When TLSF detects bad alignment, it calls `printf()` or `assert()`. Because FreeRTOS is not running during early boot, standard library functions attempt to acquire an uninitialized VFS mutex and **deadlock the CPU**. This perfectly explained the "silent" nature of the hang.
*   **Fix**: I forcefully aligned the pointer by shifting it 4 bytes forward before passing it to TLSF, and I wrapped `abort`, `__assert_func`, `__assert_fail`, and `printf` to safely route their outputs through raw `esp_rom_printf`.

#### 6. The Linker Alias Bug
Even after fixing the alignment and wrapping the panics, `__real_multi_heap_register` still hung silently.
*   **Discovery**: `multi_heap_register` in ESP-IDF is a weak linker alias to `multi_heap_register_impl`. Using `--wrap` on an alias completely breaks GCC's symbol resolution. `__real_multi_heap_register` was jumping to a corrupted/invalid memory address, trapping the CPU.
*   **Fix**: I completely bypassed `__real_multi_heap_register`. I natively implemented the metadata setup directly in my shim and called `tlsf_create_with_pool` myself.
*   **Result**: Massive success. All three primary memory regions (RAM, RTCRAM, and TCM) successfully registered without hanging.

#### 7. The `tlsf_size` Crash
After fixing `multi_heap_register`, the system needed to accurately populate the `free_bytes` field in the heap metadata by subtracting the TLSF control structure overhead. I dynamically calculated this using `tlsf_size()`.
*   **Discovery**: The boot process hung silently immediately upon calling `tlsf_size()`. This occurred because ESP32-P4 stores its TLSF implementation in ROM to save space. While `tlsf_create_with_pool` is exported properly, `tlsf_size` was either missing from the ROM export table or inappropriately compiled out, causing a silent jump to a garbage address.
*   **Fix**: I abandoned calling `tlsf_size` completely and hardcoded a conservative 4KB overhead subtraction, flawlessly tricking ESP-IDF into believing it knew the exact free size without crashing.

#### 8. `multi_heap_malloc` and the Linker Alias Bug Strikes Again
With `multi_heap_register` completely bypassed and the TLSF pools initialized, the boot sequence proceeded to allocate the permanent `heaps_array` tracking structure using `multi_heap_malloc()`.
*   **Discovery**: It crashed silently yet again! Because I was tracing `multi_heap_malloc` using `--wrap`, I inadvertently broke its symbol resolution just like `multi_heap_register`, as it is also a weak alias to `multi_heap_malloc_impl`.

#### 9. Proving Flash Execution & MMU State
I temporarily replaced the alias wrapper with one that directly called the native `tlsf_malloc` function, but it *still* silently hung the CPU.
*   **Hypothesis**: Because I bypassed `esp_mmu_map_init()` earlier, I suspected the Flash MMU was still mapped to the bootloader's partition instead of the application's. If `tlsf_malloc` (compiled into `libheap.a` in Flash) was executed, it would read garbage and crash.
*   **Test**: I wrote a probe in IRAM to manually read a 32-bit word directly from a Flash memory address (`0x4825fc58`).
*   **Result**: The read succeeded and returned `0xf66347cd` (a valid instruction). This massively important breakthrough proved that the custom bootloader's MMU mapping *was perfect*, and Flash execution was fully operational!

#### 10. The `tlsf_malloc` Mystery and the Dummy Allocator
Despite Flash memory working, calling the native `tlsf_malloc` still silently froze the CPU (likely due to a hidden assertion deadlock inside TLSF, an unaligned memory trap emitted by the compiler, or an uninitialized BSS variable). 
*   **Current Fix**: Because `heap_caps_init` only calls `multi_heap_malloc` once to allocate a tiny 180-byte array, I stopped fighting the highly-optimized ESP-IDF allocator. I replaced my wrapper with a **static dummy allocator** that just hands ESP-IDF a pointer to a pre-allocated 512-byte buffer in SRAM.
#### 11. The `.data` Section Revelation
While attempting to bypass the `tlsf_malloc` crash with a static dummy allocator, I encountered an absurd error: the dummy allocator instantly returned "OUT OF MEMORY" despite having plenty of space.
*   **Discovery**: The allocator tracked its usage with a static variable `dummy_offset` initialized to 32. In C, statically initialized variables are placed in the `.data` section. Because the custom stateless bootloader does not copy the application's `.data` section from Flash to SRAM before jumping to the application, **every initialized global variable in the entire system is full of random garbage**. 
*   **Implication**: This is a massive revelation. `tlsf_malloc` and many other ESP-IDF libraries rely heavily on initialized global variables. The fact that the entire `.data` section is uninitialized is almost certainly the root cause of the unexplained crashes.
#### 12. Isolating the End of `heap_caps_init`
The dummy allocator successfully supplied static memory to `heap_caps_init`, bypassing the `tlsf_malloc` crash! However, the CPU *still* hung silently without returning from `heap_caps_init`. 
*   **Discovery**: The `heap_caps_init` function completes its heavy lifting and only executes a small loop at the very end: it calls `multi_heap_set_lock` and `sorted_add_to_registered_heaps` for each region. 
*   **Implication**: If it didn't return, it means one of these two extremely simple functions is hanging, likely because of garbage memory (perhaps the uninitialized `.data` section corrupted something else). 
#### 13. The Ultimate Blocker: The Bootloader Overwrites `.data`
With the `multi_heap_set_lock` wrapper in place, I proved that the entire `heap_caps_init` function completes successfully! 
However, the system immediately hung on the very next initialization function (likely `esp_timer_init_nonos` or `init_libc`).
*   **Discovery**: The `seedsigner_bootloader_p4_stateless_os` bootloader *is* reading the `.data` segment from `micropython.bin` and correctly copying it to internal SRAM (`0x4FF00000` region).
*   **The Bug**: Immediately after copying the segments to SRAM, the bootloader runs a loop to thrash the 64 KB L1 D-cache:
    ```c
    // Thrash full 64 KB L1 D-cache to force eviction before MMU remap
    volatile uint32_t *evict_ptr = evict_buf;
    for (int i = 0; i < (65536 / 4); i++) evict_ptr[i] = i;
    ```
    `evict_buf` is a static 64 KB array allocated in the bootloader's own `.bss` section (which lives in internal SRAM). Because the application *also* runs from internal SRAM, the bootloader's `.bss` section overlaps with the application's `.data` section that was just copied into SRAM. By writing to `evict_buf` *after* the copy, the bootloader completely overwrites and destroys the `.data` segment of the application!
*   **Resolution**: I have updated `seedsigner_bootloader_p4_stateless_os/main/main.c` to swap the order: I now run the cache eviction loop *before* copying the application segments into SRAM. This ensures the `.data` segment is safely preserved before jumping to the application entry point.

#### 14. Native Heap Restored
After fixing the bootloader to preserve `.data`, the system booted successfully! 
*   **Result**: The trace showed that `heap_caps_init` completed flawlessly and returned to the ESP system init sequence! The system progressed through 11 more initialization functions until it naturally hit an `OUT OF MEMORY` error in my tiny 512-byte dummy allocator (which I expected).
*   **Next Step**: Because the `.data` section is now fully initialized, the native ESP-IDF `tlsf_malloc` and `multi_heap_register` functions (which rely on initialized globals) should work perfectly without hanging! I have now stripped all the dummy allocator wrappers from `stateless_shim.c` and `CMakeLists.txt` to let the native ESP-IDF heap initialization run.

#### 15. The Final Milestone: RTOS Boot
With all native functions restored and `.data` correctly preserved by the bootloader, the system achieved the ultimate milestone:
```
cpu0: secondary init complete!
cpu0: jumping to esp_startup_start_app...
```
This is the final line of ESP-IDF hardware and core initialization. `esp_startup_start_app` jumps directly into the FreeRTOS scheduler, which in turn launches the MicroPython `main_task`. The early bootloader/hardware-init phase is officially **100% stable and operational**.

*   **Next Steps**: I had several UART/logging wrappers (`esp_rom_uart_set_clock_baudrate`, `esp_rom_output_tx_wait_idle`, `printf`, `abort`, etc.) active in `stateless_shim.c` for debugging the silent hang. These wrappers were preventing MicroPython's standard console output (REPL) from appearing on the UART after FreeRTOS started. I have now scrubbed *all* debugging intercepts from `CMakeLists.txt` and `stateless_shim.c`. Rebuilding one last time will allow MicroPython's REPL to natively output to the serial console!

#### 16. The Watchdog Timer (WDT) Reset Issue
After scrubbing all wrappers, the firmware successfully completed core and secondary init sequences but crashed exactly on `esp_startup_start_app()`. The serial console displayed `rst:0x7 (HP_SYS_HP_WDT_RESET)`.
*   **Root Cause**: By manually reimplementing `call_start_cpu0` and intentionally skipping static initialization functions like `sys_rtc_init` and `system_early_init`, I failed to configure the ESP32-P4's system timers and interrupt matrices. When FreeRTOS started in `esp_startup_start_app`, it deadlocked waiting for a timer tick that never arrived, which subsequently triggered the system watchdog.

#### 17. Reverting to Native Boot and Linker Errors
*   **Attempt**: I tried to reimplement the full initialization sequence inside the shim while keeping `system_early_init` to fix the timer issue.
*   **Symptom**: Build failures with undefined references to `esp_cpu_intr_set_ivt_addr`, `sys_rtc_init`, and `system_early_init`.
*   **Root Cause**: The MicroPython Docker build environment links against a precompiled `libesp_system.a`. In this precompiled archive, `system_early_init` is strictly `static`, and `esp_cpu_intr_set_ivt_addr` is a `FORCE_INLINE` macro from `esp_cpu.h`. Recreating the native initialization manually was not viable because I couldn't link against these internal static dependencies.

#### 18. The C Compiler Prologue Trap
*   **Attempt**: I instructed the shim to enable the FPU and then manually tail-call the native ESP-IDF `call_start_cpu0`, wrapping dangerous hardware resets instead of reimplementing everything.
*   **Symptom**: The system hung instantly before making the jump.
*   **Root Cause**: `my_entry_point` was a standard C function compiled with the hardware float ABI (`-mabi=ilp32f`). The C compiler automatically inserted a "prologue" that attempted to save floating-point registers to the stack. Because this prologue executed *before* my inline assembly enabled the FPU, the CPU encountered a float instruction while the FPU was disabled, causing an immediate Illegal Instruction trap.

#### 19. The Three-Stage Boot Architecture
To safely bridge the stateless bootloader into MicroPython, I redesigned the shim into a failsafe 3-stage pipeline:
1. **Stage 1 (`my_entry_point`)**: Declared `__attribute__((naked))` to forbid compiler prologues. It uses pure assembly to instantly turn on the FPU, set the global pointer (`gp`), initialize the stack pointer (`sp`) to a dedicated `.data` array, and tail-jump to Stage 2.
2. **Stage 2 (`__wrap_call_start_cpu0`)**: Now operating in a safe C environment, it safely calls ROM functions (like `Cache_Invalidate_All`) and flushes UART diagnostic logs.
3. **Stage 3 (`__real_call_start_cpu0`)**: Execution is handed off to the native ESP-IDF boot flow to handle IVT, clocks, and FreeRTOS properly. Meanwhile, `--wrap` stubs (e.g. `esp_rtc_init`) intercept and block native ESP-IDF functions from resetting the bootloader's PSRAM/Flash/PMU states.

#### 20. The Linker Dropping `cpu_start.c.obj`
*   **Symptom**: The 3-stage boot successfully cleared Stage 1 and Stage 2 but jumped to `0x4ff05afe: esp_psram_impl_enable` instead of the real ESP-IDF boot routine, immediately crashing.
*   **Root Cause**: Because I changed the entry point to `my_entry_point` in `CMakeLists.txt`, the linker no longer viewed the native `call_start_cpu0` as the program root. Because it wasn't explicitly referenced before the `--wrap` intercepted it, the GNU linker discarded the `cpu_start.c.obj` file entirely to save space. With the real function missing, the linker blindly resolved my `__real_call_start_cpu0` pointer to a completely unrelated hardware function that shared a weak alias or memory section.
*   **Fix**: I added the explicit linker flag `"-u call_start_cpu0"` to force the linker to pull the real `cpu_start.c.obj` into the build, guaranteeing the proxy jumps to the correct startup function.

#### 21. The Phantom FreeRTOS Locks (BSS Not Cleared)
*   **Symptom**: After successfully bridging to the native ESP-IDF `call_start_cpu0`, the bootloader would silently hang inside `init_brownout` (specifically when attempting to acquire a lock in `esp_intr_alloc` or calling `gettimeofday`).
*   **Root Cause**: When I bypassed the native ROM loader and injected my `__wrap_call_start_cpu0` shim, I intentionally skipped calling the native `call_start_cpu0` to prevent it from running early init functions. However, the native `call_start_cpu0` is also responsible for executing `memset` to zero out the `.bss` (Block Started by Symbol) segments. Because I skipped this, every statically allocated variable and lock in ESP-IDF was filled with leftover garbage from the ESP32-P4 SRAM. The OS thought FreeRTOS was running and attempted to wait on uninitialized "mutexes" that were actually just garbage memory, causing a deadlock.
*   **Fix**: I manually extracted the linker symbols for the BSS segments (`_bss_start_low`, `_bss_start_high`, `_iram_bss_start`, `_rtc_bss_start`) and explicitly zeroed them out with `memset` inside my `__wrap_call_start_cpu0` shim before proceeding to execute the native initialization functions.

#### 22. Successful MicroPython Boot and LCD Allocation Failure
*   **Result**: With the BSS cleared, the system successfully flew through all bootloader initialization phases, launched the FreeRTOS scheduler, and jumped into MicroPython's `app_main()`!
*   **New Issue**: Inside MicroPython, the system attempts to initialize the `Waveshare ESP32-P4 WiFi6 Touch LCD 4.3` via an MIPI-DPI driver. The driver fails with `lcd.dsi.dpi: esp_lcd_new_panel_dpi(226): no memory for frame buffer` and triggers a hardware watchdog reset. I am now investigating where the board initialization components are located and why PSRAM allocation for the frame buffer is failing.

#### 23. LCD Allocation Failure (PSRAM Software Initialization Skipped)
*   **Symptom**: During MicroPython's display initialization, the MIPI-DSI driver crashed with `esp_lcd_new_panel_dpi(226): no memory for frame buffer`.
*   **Root Cause**: In my previous `stateless_shim.c`, I was manually iterating over `_esp_system_init_fn_array` to initialize the system and entirely skipping the native `call_start_cpu0` and `system_early_init`. Because `system_early_init` handles calling `esp_psram_init`, PSRAM was completely absent from the software heap allocator. The display driver subsequently failed to allocate the frame buffer because it required PSRAM.
*   **Fix**: I refactored `__wrap_call_start_cpu0` to drop the manual function iteration and instead simply tail-call the native `__real_call_start_cpu0`. The native routine properly clears the BSS (making my manual BSS zeroing redundant) and invokes `system_early_init`. I then removed the linker wrappers for `esp_psram_init`, `esp_mmu_map_init`, and `cache_hal_init` to allow software initialization of PSRAM, while keeping hardware-destructive functions like `esp_psram_chip_init` and `mspi_timing_flash_tuning` wrapped. This preserves the bootloader's hardware hand-off state while fully setting up the FreeRTOS OS environment.

#### 24. Conclusion of the Bootloader Saga
After resolving the linker dropping issue with `-u call_start_cpu0`, the custom 3-stage boot pipeline executed successfully. By clearing `.bss` properly in the shim and preserving the MMU configurations, I achieved a perfect, silent handoff from the stateless custom bootloader to the ESP-IDF FreeRTOS scheduler, which successfully started the MicroPython environment. This confirmed the stretch goal of running MicroPython on top of my custom app loader was fully achieved. From this point forward, the environment was deemed 100% stable, allowing me to pivot strictly to application-level hardware debugging (specifically the MIPI-DSI LCD driver allocation errors).

#### 25. Uncommitted Fixes, Flash Subsystem Crashes, and LCD Allocation Failures
*   **Symptom 1**: The final WDT fix (tail-calling `__real_call_start_cpu0` instead of looping through `init_fn`) and the `"-u call_start_cpu0"` linker flag were implemented in the working directory but left uncommitted. A subsequent `git restore` wiped these changes out, resurrecting the WDT reset.
*   **Fix 1**: I rewrote `stateless_shim.c` to accurately execute the 3-stage boot architecture (tail-calling `__real_call_start_cpu0`) and successfully committed it alongside the `-u` linker flag in `CMakeLists.txt`.
*   **Symptom 2**: The ESP-IDF Flash subsystem (VFS/FAT partition loading) and SPI flash driver violently crashed with a Guru Meditation Error (Store access fault) right before FreeRTOS app_main.
*   **Root Cause**: The shim aggressively stubbed `esp_mmu_map_init`. However, `esp_mmu_map_init` strictly initializes the ESP-IDF software MMU structures (`s_mmu_ctx`) without touching the hardware MMU. Because it was stubbed, the software had no virtual address tracking maps, causing `load_partitions` to fail and the flash driver to crash.
*   **Fix 2**: Removed the `--wrap` for `esp_mmu_map_init` in `CMakeLists.txt` and `stateless_shim.c`, permitting ESP-IDF to safely establish its software MMU context and read the partition table.
*   **Symptom 3**: The LCD MIPI-DSI DPI driver failed with `esp_lcd_new_panel_dpi(226): no memory for frame buffer` due to `heap_caps_calloc` rejecting the request.
*   **Root Cause**: I purposefully stubbed `esp_psram_init` to prevent hardware resets. Consequently, ESP-IDF's heap allocator never registered the 32MB PSRAM chunk, returning 0 bytes available for `MALLOC_CAP_SPIRAM`. Even if it were registered, ESP-IDF's ESP32-P4 memory layout doesn't assign `MALLOC_CAP_DMA` to PSRAM by default, causing the DPI driver's explicit request for both to fail.
*   **Fix 3**: I repurposed `__wrap_esp_psram_init` to manually inject the 32MB PSRAM region (0x48000000 on ESP32-P4) natively into ESP-IDF's heap allocator using `heap_caps_add_region_with_caps` with proper capabilities `(1<<10) | (1<<12) | (1<<20)`. Additionally, I added a `__wrap_heap_caps_calloc` interceptor that silently strips the `MALLOC_CAP_DMA` flag whenever `MALLOC_CAP_SPIRAM` is requested.

#### 26. The PSRAM XIP Trap
*   **Symptom**: After successfully removing the `esp_mmu_map_init` and `spi_flash_init_chip_state` wrappers to fix the Flash filesystem crash, the board hung silently right after `mmu_psram: .text xip on psram`.
*   **Root Cause**: The MicroPython firmware was built with `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` enabled. This means ESP-IDF's native `esp_psram_init` routine maps the `.text` and `.rodata` sections to PSRAM instead of Flash. Normally, the 2nd stage bootloader copies these sections into PSRAM. However, because my custom "stateless OS" bootloader bypassed the 2nd stage bootloader, it never copied `.text` and `.rodata` into PSRAM, leaving the PSRAM empty. When the CPU attempted to fetch the next instruction from the newly mapped PSRAM, it fetched garbage and hung.
*   **Fix**: I re-introduced the `__wrap_esp_psram_init` interceptor. By preventing ESP-IDF's native PSRAM initialization from running, I stop it from re-mapping `.text` and `.rodata` to PSRAM, forcing the MMU to continue executing from Flash exactly as my custom bootloader set it up. Inside the interceptor, I manually inject the 32MB PSRAM block (`0x48000000`) into the heap allocator with the proper natively-aligned capability array `{(1<<10) | (1<<12), 0, (1<<2) | (1<<1) | (1<<20)}` so that MicroPython can still use it for dynamic allocation.

#### 27. The Headless ST7701 MIPI-DSI Hardware Initialization Wedge
Prior to fixing the LVGL pipeline crash, the system was hanging earlier in the boot sequence inside `board_display_st7701_init()`.
*   **Symptom**: The system would hang silently and eventually trigger a WDT reset (`HP_SYS_HP_WDT_RESET`) during MIPI-DSI initialization.
*   **Root Cause**: The ST7701 driver attempts to configure MIPI-DSI lanes and perform hardware queries. Without a physical screen connected to acknowledge the DSI commands, the driver initialization routine hung indefinitely.
*   **Fix**: I modified `board_display_st7701.c` to gracefully abort the hardware initialization by inserting an early return:
    ```c
    ESP_LOGI(TAG, "Skipping ST7701 MIPI-DSI display init (no screen connected)");
    *io_handle = NULL;
    *panel_handle = NULL;
    return;
    ```
    This successfully unblocked the main thread, prevented the DSI-level WDT reset, and correctly fed `panel_handle = NULL` to `board_init.c` (which subsequently required the LVGL pipeline fixes in Section 29).

#### 28. The Headless GT911 Touch Controller WDT Reset
After successfully bypassing the ST7701 display initialization in headless mode, the system immediately hit a Watchdog Timer (WDT) reset (`HP_SYS_HP_WDT_RESET`) during the very next initialization step: the GT911 Touch Controller.
*   **Symptom**: The GT911 driver logged `I2C transaction unexpected nack detected` and `GT911 read error!`, repeatedly attempting to retry initialization. The blocking nature of these failed I2C queries on a missing hardware bus ultimately tripped the system watchdog.
*   **Root Cause**: `board_init.c` indiscriminately called `board_touch_gt911_init()`, which blindly polled the non-existent I2C touch controller. The I2C timeout / retry loop took too long and starved the FreeRTOS idle task, causing the WDT to fire.
*   **Fix**: I modified `board_init.c` to gracefully skip the `board_touch_gt911_init()` call entirely if no screen/touch device is present. I explicitly assigned `touch_handle = NULL` and relied on existing `if (touch_handle != NULL)` checks inside `lvgl_port_setup` to securely bypass touch driver registration in LVGL.
*   **Result**: The WDT resets completely vanished. The system now flawlessly executes a 100% headless boot directly into the MicroPython REPL via the custom stateless bootloader!
#### 29. The Console UART Wedge and the NULL-Panel LVGL Display Crash
The bootloader/OS handoff was now stable, but the MicroPython app itself froze the whole CPU ~7340ms into app boot — every boot, at the same wall-clock moment, right after `I (7347) board: Board initialized (landscape=1).` with no Guru Meditation and no reboot. This was on the stateless PSRAM-XIP build with **no LCD connected** (the driver logs `Skipping ST7701 MIPI-DSI display init (no screen connected)`).

*   **Work**: Multiple instrumentation rounds across `board_init.c` and `display_manager.cpp`. The wedge proved **time-based, not print-call-based** — four different output mechanisms (driver `ESP_LOGI`, newlib `vprintf`, direct `esp_rom_printf` FIFO-poll, and raw UART0 TX-FIFO writes) all died at the same ~7340-7347ms regardless of which calls were nearby.
*   **Discovery 1 — RTC/NOINIT stage tracking is clobbered by the bootloader**: A `RTC_NOINIT_ATTR` variable is silently zeroed every boot. The linker places `.rtc_noinit` at `0x50108888`, which is exactly where the stateless bootloader re-copies the app's 32-byte `.rtc.data` segment on every boot (`Seg 6: addr=0x50108888 len=32 -> direct copy to 0x50108888`). **Raw** LP RAM at `0x50109000` survives the bootloader's reset (a `0xa5a5a5a5` sentinel persisted across a flash+reset cycle) — stage tracking now lives there.
*   **Discovery 2 — the `uart_ll` non-blocking putc silently drops everything**: Writing to the UART0 TX FIFO via `uart_ll_get_txfifo_len`/`uart_ll_write_txfifo` never produced a single byte on this target. `esp_rom_printf` (blocking FIFO-poll) works. Only ever trust the `esp_rom_printf` path.
*   **Discovery 3 — the shim-injected PSRAM heap region is mapped and writable**: A direct read/write probe at `0x48800000` (the manually-added 32MB heap region) succeeded, ruling out an MMU/unmapped-page fault for the crash.
*   **Root Cause — the NULL-panel LVGL display**: `lvgl_port_setup()` in `board_init.c` — the ST7701 **landscape** branch created the full LVGL display pipeline unconditionally: two 768KB SPIRAM draw buffers + two rotation buffers + a dedicated flush task + custom rotate/flush callbacks, even though `panel_handle == NULL` (only the DPI event-callback registration was NULL-guarded). The LVGL handler task started at ~7335ms and its **first** `lv_timer_handler()` dispatch (~7340ms, right after `Board initialized`) rendered a full frame into the PSRAM buffers and flushed it to a NULL panel. A priority-2 heartbeat task (started at boot, above every other task, all of which are prio 1) stopped beating at exactly that moment — proving a CPU-wide panic-halt (or masked spin), not a starved main task. No Guru Meditation appeared despite `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y`, so the panic print itself presumably wedged on the dead UART.
*   **Fix**:
    *   `board_init.c`: guard the ST7701-landscape display/flush creation with `if (panel_handle != NULL)`; when there's no panel, `*disp_out = NULL` and the esp_lvgl_port task idles (`esp_lvgl_port.c` skips `lv_timer_handler()` when `lv_display_get_default()` is NULL). Relaxed the display assert to `assert(*disp_out != NULL || panel_handle == NULL)`.
    *   `display_manager.cpp`: tolerate `lvgl_disp == NULL` — skip `set_display()`, `overlay_manager_init()`, and the splash `run_screen()` when headless, letting the board boot straight to MicroPython.
*   **Result**: With the guard, the board boots cleanly into the MicroPython REPL and the heartbeat kept beating past the old freeze point and into the REPL — confirming the CPU never wedged and the NULL-panel first-refresh was definitively the crash. Both changes are permanent fixes (building an LVGL pipeline for a non-existent panel was simply wrong, and headless tolerance is required for this bare-P4 dev build).
*   **Retained**: the cross-boot stage recorder (`p11_stage()` → raw LP RAM `0x50109000`, reported on the next boot as `[P11-recover] prev_boot_stage=N`, stages 1-9). The heartbeat, PSRAM probe, and marker prints were stripped after the fix to keep the REPL clean. One note: LP RAM content was garbage (`0x3201c7f8`) after one particular power-cycle between test boots, so persistence across the bootloader's reset is reliable, but not across a full power loss.

#### 30. The REPL-UART RX No-Op: boot banner prints but keystrokes never land
The headless boot now reached the REPL, but input was dead: the banner printed, the cursor sat at `>>>` (and stopped blinking while typing), and nothing echoed — no output at all, no `NameError`, no line editing. TX worked fine the whole time.

*   **Root Cause**: the stateless shim link-wraps MicroPython's `uart_stdout_init()` (`-Wl,--wrap=uart_stdout_init`) and the wrap was an **empty no-op**. That function (`ports/esp32/uart.c`) is what installs the UART0 **RX** interrupt — `esp_intr_alloc` for `uart_irq_handler`, RX-FIFO-full + RX-timeout thresholds, RX interrupt enable — feeding `stdin_ringbuf`. With it gutted, `uart_stdout_tx_strn()` still worked (raw `uart_hal_write_txfifo` on the bootloader-configured UART at 115200), but no byte ever reached the ring buffer, so `mp_hal_stdin_rx_chr()` blocked forever. Classic one-way UART: output yes, input nothing.
*   **Why it was wrapped at all**: to stop MicroPython's stock init from calling `uart_hal_init()` and resetting the UART peripheral the bootloader already configured (the "console UART wedge" fear). The fix keeps that intent — it wires up RX **without** resetting the UART.
*   **Fix**:
    *   `stateless_shim.c` `__wrap_uart_stdout_init()`: register the REPL ISR (`esp_intr_alloc` on `uart_periph_signal[0].irq` with `ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_IRAM`), set the RX-FIFO-full/timeout thresholds, and enable `UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT` — no `uart_hal_init()`, no baud/pin changes. On allocation failure it prints `[P11] uart_stdout_init: esp_intr_alloc failed <n>` and continues.
    *   `ports/esp32/uart.c`: exported `uart_irq_handler` (dropped `static` on the forward decl + definition) so the shim can register it.
    *   shim `CMakeLists.txt`: added `REQUIRES esp_hw_support soc hal esp_rom heap esp_common` for the new headers (`esp_intr_alloc.h`, `soc/uart_periph.h`, `hal/uart_hal.h`, `hal/uart_ll.h`). (First attempt listed `common` as a component name — that is not valid; `REQUIRES` must name real components.)
*   **Result**: verified on hardware — `5+6` → `11`, `print("Summer of Bitcoin+SeedSigner")` prints, so the full end-to-end state (stateless boot → headless board init → REPL with working input) is now solid. If RX ever dies again, `esp_intr_alloc` failing is the first suspect (it lazily triggers the CLIC interrupt-allocator init that the shim's custom boot bypasses).

#### 31. Clean rebuild fails: "patch does not apply" on the stock integration patch
After the changes were committed and the MicroPython submodule was pinned at a locally-patched commit, a clean `make docker-build-all` failed: `0001-esp32-integration-mods.patch` would not apply (`error: patch failed ... patch does not apply` on all 8 files) and the build aborted.

*   **Root Cause**: the builder's patch system assumes the submodule is pinned at the **clean upstream base**. `apply_micropython_mods.sh` applies `mods/patches/0001-*.patch` + the `mods/new_files/` overlay, commits the result as "seedsigner-builder: applied patch series + board overlay", and `restore_micropython_clean.sh` resets back to the pin afterwards. By pinning the submodule at a commit that **already contained** the patch + overlay + manual edits, `git apply` re-ran against a tree that already had every hunk applied → context mismatch on every file. (The earlier successful builds had only worked because the tree was *dirty*, which makes `apply_micropython_mods.sh` skip the apply step entirely.)
*   **Fix**: restore the intended contract —
    *   Re-pin `deps/micropython/upstream` back to the stock base `78ff170de` (v1.27.0; the base repo already pins this, so **no micropython submodule commits are needed and nothing needs to be pushed there**).
    *   Regenerate `0001-esp32-integration-mods.patch` via `scripts/generate_micropython_patch.sh` so it now carries the two `main.c` bootloader hand-edits (NVS init commented out, flash size hardcoded to 16 MB, vfs auto-create commented) and the `uart.c` `uart_irq_handler` export on top of the stock builder changes. Shim, board files, and partitions stay in the `new_files/` overlay (excluded from the patch).
    *   Dropped the earlier `machine_wdt.c` `#if CONFIG_ESP_TASK_WDT_EN` guard — checked ESP-IDF 5.5.1: `esp_task_wdt.h` declares and `esp_system/task_wdt/task_wdt.c` implements the `esp_task_wdt_*` API **unconditionally**, so stock `machine_wdt.c` compiles/links fine with the task WDT disabled; the guard was unnecessary. **THIS WAS WRONG** — the header declares unconditionally, but `esp_system/CMakeLists.txt` only compiles `task_wdt/task_wdt.c` into the build when `CONFIG_ESP_TASK_WDT_EN` is set, so with the option off the `esp_task_wdt_*` symbols are absent from the link. My rebuild failed at final link with `undefined reference to esp_task_wdt_reconfigure / esp_task_wdt_add_user / esp_task_wdt_reset_user` (in `machine_wdt.c.obj` of `esp-idf/main/libmain.a`). Reinstated the guard as `deps/micropython/mods/new_files/ports/esp32/machine_wdt.c` (overlay copy; the patch generator excludes overlay paths so the patch itself is untouched) — commit `56b9a86` in the final history below.
    *   Reorganized the builder repo into 4 clean commits on top of upstream `f487816`: `e988eb1` (shim + board config + regenerated patch), `02bdf7c` (headless `display_manager`), `56b9a86` (machine_wdt WDT guard), `8b33304` (point the `board_common` submodule at `wolgwang1729/esp-board-common`, branch `stateless-boot`, since the stateless-boot commits are not on the upstream board_common repo). Upstream gitlink unchanged from stock.
*   **Verification**: a simulated clean build (fresh `78ff170de` worktree + regenerated patch + `new_files/` overlay) diffed against the last-known-good tree is **identical** except a cosmetic comment line in `sdkconfig.board` ("32MB flash" vs "16MB flash"). The reinstated `machine_wdt.c` guard matches the original hardware-verified firmware. I then ran a clean `make docker-build-all` against the final commit set and it **passed end-to-end** (patch apply → compile → link), confirming the stateless-boot firmware now builds cleanly from the builder repo with no local MicroPython submodule commits.

## 5. Statelessness Audit: Flash-Write Path Review

**Verdict: the boot chain never writes or erases onboard SPI flash at any point from power-on through the MicroPython REPL.** The "stateless" property is not just an architectural claim — a write/erase/`nvs` grep over the loader and shim returns zero matches, and every ESP-IDF/MicroPython subsystem that could write flash at runtime is either compiled out, patched out, or finds no target partition.

Definition in force: no write/erase to internal flash during boot or runtime; the payload executes from PSRAM only (IROM/DROM MMU-mapped to PSRAM pages, `.data` copied to internal SRAM by the loader).

Layer-by-layer evidence:

- **ROM + ESP-IDF 2nd-stage bootloader (`0x2000`).** Note this bootloader *is* part of the chain — it loads the loader app from `0x30000`; "the loader never runs the 2nd-stage bootloader" refers to the *payload* jump. The P4 bootloader (`components/bootloader_support/src/esp32p4/bootloader_esp32p4.c`) only touches registers/analog config — no `esp_rom_spiflash_write`, no WRSR, no erase. Loader `sdkconfig` keeps it that way: no OTA, no `BOOTLOADER_FACTORY_RESET`/`APP_ROLLBACK`/anti-rollback/secure-boot/flash-enc, `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`, and DIO flash mode (no QIO-mode status-register write).
- **Loader app (`main/main.c`).** `app_main` does `esp_partition_read` (read-only), PSRAM allocations, memcpy, MMU/register writes, cache flush. No `esp_flash_write`/`esp_partition_write`/`erase`/`nvs_*`/WRSR anywhere; NVS is not even linked (`PRIV_REQUIRES esp_mm spi_flash esp_partition`).
- **Hand-off** (`do_mmu_mapping_and_jump`): WDT/interrupt/PMP teardown, D-cache eviction, SRAM segment copy, MMU page-table writes, `fence.i` — RAM/register only. `RTC_DATA_ATTR` state lives in RTC RAM (not flash) and is re-initialized each boot.
- **`stateless_shim.c` + native `__real_call_start_cpu0`.** `spi_flash_init_chip_state` is a no-op on P4 in non-OPI mode (`components/spi_flash/flash_ops.c:179` returns `ESP_OK` doing nothing); `esp_mmu_map_init`, `esp_clk_init`, `esp_perip_clk_init` are software/register-only; PSRAM heap injection uses `heap_caps_add_region_with_caps` (RAM).
- **MicroPython runtime.** `nvs_flash_init()` is commented out in the integration patch — NVS is never initialized. Flash VFS creation is commented out and the flash size is stubbed, so `_boot.py` finds no `vfs`/`ffat` partition (`bdev=None`): no mount, no `inisetup.setup()` format. v1.27.0 has no `machine.Flash`. SD writes go to the removable card, not onboard flash.

Known latent surfaces (inactive today, documented deliberately):

1. **Coredump-to-flash in the payload build** (`sdkconfig.board:133`, `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`). Benign because the *loader's* partition table (`partitions.csv`) has no `coredump` partition, so `esp_core_dump_flash_init` self-disables. **Kept intentionally** for this phase's SD-card-free testing loop; to be switched to `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE` in the next phase.
2. **`esp32.Partition.writeblocks` and `esp32.NVS` stay compilable.** NVS is inert (never initialized, `nvs_open` fails); `Partition.writeblocks` can target the `payload` partition (subtype `0xFF`) on an explicit Python call. Boot path is write-free; a *compromised* runtime could still write on demand. Decision: keep the APIs — active anti-persistence is the Phase 7 flash-fill + anti-phishing layer, not this loader.
3. **Flash-size lie in the shim** (`esp_flash_default_chip->size` hardcoded 16MB on an 8MB chip). Inert with VFS creation disabled; a footgun for any future "write to free space" logic.

## 6. Final Result

**PASS — MicroPython runs on the custom stateless app loader, end-to-end verified on hardware.**

The complete chain now works on the Waveshare ESP32-P4 (no LCD connected):

```
stateless bootloader (flashes app @0x10000, jumps to my_entry_point)
        →  stateless_shim (FPU, .bss, clocks, MMU, PSRAM heap, watchdogs)
        →  headless board_init (ST7701/GT911 skipped, NULL-panel LVGL guarded)
        →  MicroPython REPL with working UART input
        →  5+6 → 11
```

Highlights:

- **Boot hand-off:** All 31 debugging milestones resolved (see the Silent Heap Hang log above) — the shim now tail-calls the native `__real_call_start_cpu0` with `--wrap` stubs only for the hardware-destructive routines, so the bootloader's PSRAM/flash/PMU state is preserved while ESP-IDF's software init (heap, MMU context, clocks) runs normally.
- **Headless operation:** No-LCD tolerance is a permanent fix — the ST7701/GT911 init is skipped, the LVGL pipeline is not built for a NULL panel, and `display_manager` skips `set_display()`/`overlay_manager_init()` when no display is present.
- **REPL input:** `uart_irq_handler` is exported and wired up by the shim's `__wrap_uart_stdout_init()` so keystrokes reach the ring buffer (the "cursor at `>>>` with no echo" bug is gone).
- **Reproducibility:** The MicroPython submodule stays pinned at clean stock `78ff170de`; all changes travel via the regenerated `0001-esp32-integration-mods.patch` + `new_files/` overlay. A clean `make docker-build-all` passes with no `MP_ALLOW_DIRTY=1`. Final builder repo: 4 commits on upstream `f487816` (`e988eb1` → `02bdf7c` → `56b9a86` → `8b33304`), pushed to `wolgwang1729/seedsigner-micropython-builder`; `board_common` pinned to `151c6d8` on `stateless-boot`.

Artifacts: `run_micropython.sh` (flash + monitor) and the full change-set are documented in `seedsigner_micropython_builder_changes/README.md`.

