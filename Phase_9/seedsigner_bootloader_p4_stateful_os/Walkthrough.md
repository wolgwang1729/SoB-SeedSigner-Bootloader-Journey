# ESP32-P4 Secure Bootloader Prototype - Debugging Walkthrough

This document outlines the major bugs encountered, the root causes discovered, and the solutions implemented during the 3-day bring-up of the Phase 9 ESP32-P4 Secure Bootloader prototype. It also covers misunderstandings that occurred and the pending steps required to finalize the bootloader.

## 1. Cache Controller Hang on L1 D-Cache Writeback
* **The Bug:** After copying the payload segments into RAM and Flash, the bootloader would hang silently when attempting to write back the L1 Data Cache to main memory before disabling the caches.
* **Root Cause:** Calling the standard ESP-IDF cache HAL functions (`Cache_WriteBack_Addr` or `cache_ll_writeback_all`) on internal SRAM segments causes a silicon-level hang on the ESP32-P4 cache controller when preparing for an MMU remap.
* **The Solution:** I bypassed the hardware cache controller commands entirely by implementing a software "cache thrashing" mechanism. I allocated a pointer deep inside unused SRAM (`0x4FF80000`) and manually wrote 32KB of dummy data to it. This forced the hardware to naturally evict all of the payload copies from the 32KB L1 D-Cache into main memory without triggering the controller bug.

## 2. ESP32-P4 Chip Revision Mismatch
* **The Bug:** Flashing or booting failed with an error indicating the chip revision is not supported: `bootloader/bootloader.bin requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)` (or similar for the `hello_world` app).
* **Root Cause:** ESP-IDF v5.5 defaults to compiling projects for production silicon (v3+). Since your physical board uses older `v1.3` pre-production silicon, the bootloader (and payload app) refused to boot.
* **The Solution:** I updated the `sdkconfig` via `idf.py menuconfig` (under Component config -> Chip revision) to enable `Select ESP32-P4 revisions <3.0 (No >=3.x Support)` (Kconfig variable: `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`). I applied this fix to both the secure bootloader project and the `hello_world` dummy payload project.

## 3. Bootloader Crash During MMU Remap (The "JMP[9]" Hang)
* **The Bug:** The bootloader successfully printed `W1 W2 OK` for all MMU pages, but completely froze or panicked before it could print `JMP[9] MMU done`.
* **Root Cause:** The string literal `"JMP[9] MMU done\r\n"` was stored in the `.rodata` section, which lives in Flash. As part of the handoff, the bootloader remapped the MMU to point to the new payload and disabled the L2 cache. When the CPU subsequently tried to print `JMP[9]`, it attempted to fetch the string from Flash. Because the L2 cache was off and the MMU mapping was altered, the fetch failed, causing an immediate Instruction/Load Fetch Panic.
* **The Solution:** I completely removed all `esp_rom_printf` calls and string literal accesses *after* the MMU remap and cache disable block. The bootloader now silently and safely executes the `fence.i` instruction and jumps to the payload.

## 4. App Image Header Magic Mismatch (`Invalid app image header`)
* **The Bug:** The bootloader successfully jumped to the payload, but the payload application (`hello_world`) immediately aborted in its own startup code (`cpu_start.c`) with the error: `Invalid app image header`.
* **Root Cause:** ESP-IDF's startup code verifies the application image by checking for a specific magic byte (`0xE9`) at the beginning of the virtual 64KB Flash page (e.g., `0x40020000`). My secure loader was extracting the flash segment data and writing it to offsets like `0x140020`, but it left the first 32 bytes of the page (`0x140000`) completely erased (`0xFFFFFFFF`). Because I skipped copying the 32-byte header, the payload's self-check failed.
* **The Solution:** I modified the segment writing loop in `main.c`. Now, whenever a flash-mapped segment (IROM/DROM) is written, I additionally copy the original 32-byte image header to the physical 64KB page boundary (`write_addr & ~0xFFFF`). This ensures the ESP-IDF startup code finds the `0xE9` magic byte exactly where it expects it.

---

## 5. Success: The Bootloader Handoff (Where did JMP[9]-JMP[12] go?)
You might notice that the final logs only go up to `JMP[8]` before jumping into the payload, whereas earlier iterations had up to 12 steps. 
**This is intentional and proves the fix for Bug #3 is working.**
By removing the debug prints for steps 9 through 12, I prevent the CPU from attempting to fetch the string literals from the flash memory after the L2 cache has been disabled and the MMU remapped. Thus, the bootloader silently and successfully executes the final steps and handoff without triggering an instruction fetch panic.

### Final Verification Logs
The following log proves that the bootloader successfully loads the payload, disables the cache, remaps the MMU, and hands off execution to the `hello_world` app:

```text
I (3562) SEEDSIGNER_LOADER: Jumping to entry point...
I (3572) SEEDSIGNER_LOADER: Copy 0: dest=0x30100000, src=0x48019ed4, len=84
I (3572) SEEDSIGNER_LOADER: Copy 1: dest=0x4ff00000, src=0x48019f30, len=25064
I (3582) SEEDSIGNER_LOADER: Copy 2: dest=0x4ff061e8, src=0x48036820, len=36880
I (3592) SEEDSIGNER_LOADER: Copy 3: dest=0x4ff0f200, src=0x4803f838, len=9228
JMP[1] entered
JMP[2] WDT disabled
JMP[3] interrupts off, starting copies
JMP[4] copies done, verifying entry point bytes: 0xCE061101
JMP[5] D-cache evicted via thrash
JMP[6] I-cache invalidate done
JMP[7] L2 cache disabled
JMP[8] MMU map: vaddr=0x40020000 paddr=0x00160000 pages=0x00000001
  entry=0x00000002 val=0x00001016 W1 W2 OK
JMP[8] MMU map: vaddr=0x40000000 paddr=0x00140000 pages=0x00000002
  entry=0x00000000 val=0x00001014 W1 W2 OK
  entry=0x00000001 val=0x00001015 W1 W2 OK
I (4819) cpu_start: Multicore app
I (4830) cpu_start: GPIO 38 and 37 are used as console UART I/O pins
I (4830) cpu_start: Pro cpu start user code
I (4830) cpu_start: cpu freq: 360000000 Hz
...
I (4937) main_task: Started on CPU0
I (4957) main_task: Calling app_main()
Hello world!
This is esp32p4 chip with 2 CPU core(s), , silicon revision v1.3, 8MB external flash
Minimum free heap size: 600124 bytes
Restarting in 10 seconds...
Restarting in 9 seconds...
```

The `Hello world!` output signifies the completion of the Phase 9 Bootloader bring-up!
