# ESP32-P4 Bootloader `main.c` — Complete Code Explanation

This document provides a complete line-by-line and conceptual breakdown of the ESP32-P4 firmware bootloader handoff code, including all diagrams from the original analysis.

---

## 1. System Architecture & Handoff Pipeline

```
┌────────────────────────────────────────────────────────┐
│                   Flash Partition                      │
└────────────────────────────────────────────────────────┘
                           │
                    esp_partition_read()
                    (SPI Flash DMA bypasses CPU cache)
                           │
                           ▼
┌────────────────────────────────────────────────────────┐
│         psram_buf (Raw Packed Binary Dump)             │
│    [image_header][seg_hdr][seg_data][seg_hdr]...       │
└────────────────────────────────────────────────────────┘
                           │
                    Cache_WriteBack_Addr()
                    (Flush D-Cache + I-Cache)
                           │
                    Parse segment headers,
                    calculate max_offset
                           │
                           ▼
┌────────────────────────────────────────────────────────┐
│         fake_flash (MMU-Aligned Staged Layout)         │
│    [image_hdr at page 0][zeros][seg_data at write_off] │
└────────────────────────────────────────────────────────┘
                           │
                    Cache_WriteBack_Addr()
                    (Flush fake_flash to physical PSRAM)
                           │
                    Configure MMU table
                           │
                           ▼
┌────────────────────────────────────────────────────────┐
│  MMU Maps: Physical (fake_flash) ──► Virtual 0x48000000│
└────────────────────────────────────────────────────────┘
                           │
                    Execute safe_copies[]
                    (IRAM/DRAM segments)
                           │
                    Jump to entry_addr
                           │
                           ▼
┌────────────────────────────────────────────────────────┐
│                   Target Firmware Runs                 │
└────────────────────────────────────────────────────────┘
```

---

## 2. Reading the Partition & Cache Invalidation

### Code
```c
esp_partition_read(part, 0, psram_buf, fw_size);

Cache_WriteBack_Addr(0x10, (uint32_t)psram_buf, fw_size);  // Flush D-Cache
Cache_WriteBack_Addr(0x20, (uint32_t)psram_buf, fw_size);  // Flush I-Cache
```

### Why `Cache_WriteBack_Addr` is Mandatory

`esp_partition_read()` uses the **SPI Flash DMA controller** to copy data directly from flash → physical PSRAM. The DMA engine **completely bypasses the CPU cache**.

```
Flash ──DMA──► Physical PSRAM    OK (correct data here)
CPU D-Cache                      NO (stale / uninitialized)
CPU I-Cache                      NO (stale / uninitialized)
```

Without flushing:
1. CPU reads from D-Cache → sees **stale garbage data**, not the firmware
2. CPU fetches instructions from I-Cache → executes **old cached opcodes**

With flushing:
```
1. esp_partition_read()
   Flash ──DMA──► Physical PSRAM  OK
2. Cache_WriteBack_Addr(0x10, ...)  → Invalidates D-Cache lines
3. Cache_WriteBack_Addr(0x20, ...)  → Invalidates I-Cache lines
4. CPU reads/executes psram_buf
   Cache miss → fetches from Physical PSRAM OK (fresh data)
```

| Parameter | Target |
|-----------|--------|
| `0x10` | D-Cache (Data Cache) |
| `0x20` | I-Cache (Instruction Cache) |

---

## 3. Pre-Read Partition Sanity Check (Early Exit)

### Code
```c
uint8_t magic = 0;
if (esp_partition_read(part, 0, &magic, 1) != ESP_OK || magic != 0xE9) {
    ESP_LOGE(TAG, "'payload' partition empty or invalid magic (0x%02X). Halting.", magic);
    while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
}
```

This reads **only 1 byte** directly from flash **before** doing the expensive full DMA read. It is a cheap early-exit guard: if the partition is empty (`0xFF`) or clearly corrupt, there is no point allocating large PSRAM buffers or doing a multi-megabyte DMA transfer.

---

## 4. Full Image Header Validation

### Binary Layout

```
psram_buf (raw bytes from flash):
┌────────┬──────────┬──────────┬──────────┬──────────────────────────┐
│  0xE9  │ seg_count│  ...     │ entry_   │  ... rest of firmware ... │
│ magic  │          │          │  addr    │                           │
└────────┴──────────┴──────────┴──────────┴──────────────────────────┘
  byte 0   byte 1                byte 4-7
```

### Code
```c
__attribute__((aligned(4))) esp_image_header_t hdr;
hdr = *(esp_image_header_t *)psram_buf;

if (hdr.magic != 0xE9) {
    ESP_LOGE(TAG, "Bad image magic: 0x%02X (expected 0xE9). Halting.", hdr.magic);
    while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
}

if (hdr.segment_count == 0 || hdr.segment_count > 16) {
    ESP_LOGE(TAG, "Bad segment count: %d. Halting.", hdr.segment_count);
    while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
}

ESP_LOGI(TAG, "Image OK: %d segments, entry=0x%08lX",
         hdr.segment_count, (unsigned long)hdr.entry_addr);
```

### Double-Asterisk Syntax Explained

In `hdr = *(esp_image_header_t *)psram_buf;` there are two `*` symbols doing completely different things:

```c
hdr = *(esp_image_header_t *)psram_buf;
      ▲        ▲
      │        └─ Asterisk 1: TYPE CAST (defines pointer type)
      └────────── Asterisk 2: DEREFERENCE (fetches the actual value)
```

Step by step:
```
psram_buf                         → uint8_t * (raw byte pointer)
(esp_image_header_t *)psram_buf   → esp_image_header_t * (struct pointer)
*(esp_image_header_t *)psram_buf  → esp_image_header_t (the actual struct value)
```

```
psram_buf ──cast──► (esp_image_header_t*)  ──deref──►  hdr (local copy)
[0xE9][...][...]                                        { .magic = 0xE9, ... }
```

### Why Check Magic Twice?

| Check | Where | Reads | Purpose |
|-------|-------|-------|---------|
| First check | Directly from flash | 1 byte only | Cheap early-exit before allocating PSRAM / DMA |
| Second check | From `psram_buf` in PSRAM | Full `esp_image_header_t` struct | Verifies DMA transfer completed correctly |

### segment_count Validation

| Condition | Meaning |
|-----------|---------|
| `== 0` | No segments → nothing to load, clearly corrupt |
| `> 16` | Espressif's spec caps at 16 maximum segments. Also prevents buffer overrun if you loop over `segment_count` later |

A typical ESP32-P4 firmware has **3–6 segments**. A value of `255` (e.g., from an erased `0xFF` partition) would cause a loop to walk far off the end of the buffer.

### `hdr.entry_addr`

The virtual address of the **first instruction to execute** after all segments are loaded — the firmware's reset vector / bootstrap. Logged at this point so you can verify it matches the expected address before actually jumping.

---

## 5. Firmware Binary Layout in Memory

```
Firmware .bin (as stored on flash and copied to psram_buf):
┌──────────────────┬────────────┬──────────┬────────────┬──────────┬─────┐
│ esp_image_header │ seg_header │ seg_data │ seg_header │ seg_data │ ... │
│  (magic, count,  │     1      │    1     │     2      │    2     │     │
│   entry_addr...) │ (8 bytes)  │          │ (8 bytes)  │          │     │
└──────────────────┴────────────┴──────────┴────────────┴──────────┴─────┘
                    ◄──── segment_count segments total ──────────────────►
```

Each `esp_image_segment_header_t` contains only two fields:
- `load_addr` (4 bytes): Virtual Address where this segment lives at runtime
- `data_len` (4 bytes): Size of the segment payload in bytes

---

## 6. Calculating the PSRAM Footprint (`max_offset`)

### Code
```c
uint32_t max_offset = 0;
uint32_t offset     = sizeof(esp_image_header_t);  // Skip image header, point to first seg header

for (int i = 0; i < hdr.segment_count; i++) {
    esp_image_segment_header_t seg;
    memcpy(&seg, psram_buf + offset, sizeof(seg));  // Read segment header
    offset += sizeof(seg);                           // Advance past segment header

    if (seg.data_len > MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Segment %d bad length %lu. Halting.", i, (unsigned long)seg.data_len);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    if (seg.load_addr >= 0x48000000 && seg.load_addr < 0x4C000000) {
        uint32_t end = (seg.load_addr - 0x48000000) + seg.data_len;
        if (end > max_offset) max_offset = end;
    }
    offset += seg.data_len;  // Advance past segment data
}

max_offset = (max_offset + 0xFFFF) & ~0xFFFF;  // Round up to 64KB boundary
ESP_LOGI(TAG, "PSRAM MMU footprint: %lu bytes", (unsigned long)max_offset);
```

### Key Concepts

- `0x48000000` – `0x4C000000` is the **ESP32-P4 PSRAM virtual address range** (16 MB window)
- `seg.load_addr` is the **Virtual Address (VA)**: where the compiler/linker decided this segment must live when the program runs. This was hardcoded at compile time in the linker script (`.ld` file).
- `max_offset` finds the byte furthest from `0x48000000` across all PSRAM segments

### 64 KB Round-Up
```c
max_offset = (max_offset + 0xFFFF) & ~0xFFFF;
```
Example:
```
max_offset = 0x31234
+ 0xFFFF  = 0x41233
& ~0xFFFF = 0x40000  ← rounded up to next 64 KB boundary
```
The ESP32-P4 MMU maps memory in **64 KB pages**. You cannot map partial pages, so the footprint must be rounded to a page boundary.

### Visual Result

```
PSRAM:
0x48000000 ├──────────────────────┤
           │                      │
           │   firmware segments  │
           │                      │
0x48000000 │                      │
+ max_offset└──────────────────────┘ ← "image ends here" (64KB aligned)
           │   unused PSRAM...    │
```

---

## 7. Two PSRAM Buffers Explained

```c
// Buffer 1: psram_buf
heap_caps_aligned_alloc(65536, fw_size, MALLOC_CAP_SPIRAM);

// Buffer 2: fake_flash
heap_caps_aligned_alloc(65536, max_offset, MALLOC_CAP_SPIRAM);
```

| Buffer | Allocation Size | Content | Purpose |
|--------|----------------|---------|---------|
| **`psram_buf`** | `fw_size` | Packed `.bin` format: image_header + seg_headers + seg_data all packed sequentially, exactly as on flash | Raw transit buffer. Source for parsing. |
| **`fake_flash`** | `max_offset` | Segments placed at their correct PSRAM offsets, headers stripped, gaps zeroed | MMU target. Execution-ready layout. |

```
psram_buf (packed):              fake_flash (sparse, at correct offsets):
┌─────────────┐                  0x0000 ┌──────────────┐
│ image_header│                         │   [empty]    │
│ seg_header  │                         │  seg_data_0  │ ← write_off
│ seg_data_0  │──parse & copy──────────►│  [gap/zeros] │
│ seg_header  │                         │  seg_data_1  │ ← write_off
│ seg_data_1  │                         └──────────────┘
└─────────────┘
```

After the segments are staged from `psram_buf` into `fake_flash`, `psram_buf` is no longer needed and can be freed.

---

## 8. Allocating `fake_flash` and Getting Its Physical Address

### Code
```c
uint8_t  *fake_flash  = NULL;
uint32_t  flash_paddr = 0;

if (max_offset > 0) {
    fake_flash = heap_caps_aligned_alloc(65536, max_offset, MALLOC_CAP_SPIRAM);
    memset(fake_flash, 0, max_offset);

    mmu_target_t target;
    esp_mmu_vaddr_to_paddr(fake_flash, &flash_paddr, &target);
    ESP_LOGI(TAG, "fake_flash: vaddr=%p paddr=0x%08lX", fake_flash, (unsigned long)flash_paddr);
}
```

### The Core Idea

Normally the MMU maps physical flash → virtual address space. Here you are making it map **physical PSRAM** instead, tricking the MMU into thinking PSRAM is "flash":

```
Normal:  Physical Flash  ──MMU──► Virtual 0x48000000
Here:    Physical PSRAM  ──MMU──► Virtual 0x48000000  ← "fake flash"
```

`esp_mmu_vaddr_to_paddr()` retrieves the **physical address** of `fake_flash` in PSRAM. The MMU is programmed with **physical addresses**, not virtual ones — so `flash_paddr` is what gets passed to the MMU table configuration.

---

## 9. The Segment Staging Loop

### Code
```c
offset = sizeof(esp_image_header_t);
mmu_mapping_t mmu_mappings[16];  // Max 16 because segment_count was validated ≤ 16
int      mapping_count   = 0;
uint32_t last_page_start = 0xFFFFFFFF;

for (int i = 0; i < hdr.segment_count; i++) {
    esp_image_segment_header_t seg;
    memcpy(&seg, psram_buf + offset, sizeof(seg));
    offset += sizeof(seg);

    ESP_LOGI(TAG, "Seg %d: addr=0x%08lX len=%lu",
             i, (unsigned long)seg.load_addr, (unsigned long)seg.data_len);

    if (seg.load_addr >= 0x48000000 && seg.load_addr < 0x4C000000) {
        // PATH 1: PSRAM segment
        uint32_t va_start = seg.load_addr & ~0xFFFF;
        uint32_t va_end   = (seg.load_addr + seg.data_len + 0xFFFF - 1) & ~0xFFFF;
        if (seg.data_len == 0) va_end = va_start;

        mmu_mappings[mapping_count].vaddr = va_start;
        mmu_mappings[mapping_count].len   = va_end - va_start;
        mmu_mappings[mapping_count].paddr = flash_paddr + (va_start - 0x48000000);

        uint32_t write_off  = seg.load_addr - 0x48000000;
        uint32_t page_start = write_off & ~0xFFFF;

        if (page_start != last_page_start) {
            memcpy(fake_flash + page_start, psram_buf, 32); // main image header at page start
            last_page_start = page_start;
        }
        memcpy(fake_flash + write_off, psram_buf + offset, seg.data_len);

        mapping_count++;
        ESP_LOGI(TAG, "  -> fake_flash+0x%lX", (unsigned long)write_off);
    } else {
        // PATH 2: Internal SRAM segment (IRAM/DRAM) — deferred
        safe_copies[safe_copy_count].dest = (void *)seg.load_addr;
        safe_copies[safe_copy_count].src  = (void *)(psram_buf + offset);
        safe_copies[safe_copy_count].len  = seg.data_len;
        safe_copy_count++;
        ESP_LOGI(TAG, "  -> direct copy to 0x%08lX", (unsigned long)seg.load_addr);
    }
    offset += seg.data_len;
}

Cache_WriteBack_Addr(0x10, (uint32_t)fake_flash, max_offset);
Cache_WriteBack_Addr(0x20, (uint32_t)fake_flash, max_offset);
```

---

### PATH 1: PSRAM Segment Mapping

#### `va_start` and `va_end` — 64 KB Virtual Page Boundaries

`seg.load_addr` (the virtual address from the linker) is not guaranteed to be 64 KB aligned. But the MMU can only map in 64 KB page units. So you must round down to the containing page:

```c
uint32_t va_start = seg.load_addr & ~0xFFFF;           // round DOWN to 64KB
uint32_t va_end   = (seg.load_addr + seg.data_len      // round UP
                     + 0xFFFF - 1) & ~0xFFFF;
```

Example with `load_addr = 0x4800C200`, `data_len = 0x5000`:
```
va_start = 0x4800C200 & ~0xFFFF = 0x48000000  ← page containing load_addr
va_end   = (0x4800C200 + 0x5000 + 0xFFFF - 1) & ~0xFFFF = 0x48020000
```

> **Note:** `fake_flash`'s physical alignment (64 KB) and `va_start`/`va_end` alignment are independent concerns. `fake_flash` alignment ensures the physical side is page-aligned; `va_start`/`va_end` ensures the virtual side is page-aligned. Both are required.

---

#### MMU Physical Address Calculation

```c
mmu_mappings[mapping_count].paddr = flash_paddr + (va_start - 0x48000000);
```

- `(va_start - 0x48000000)`: Offset from the virtual PSRAM base → how far into `fake_flash` this page starts
- `flash_paddr + offset`: The physical PSRAM address of that page

```
VIRTUAL ADDRESS SPACE (va_start)        PHYSICAL PSRAM (flash_paddr)
0x48000000 (Base)  ─────────────────── ► 0x30000000 (fake_flash base)
   │                                        │
   │  + 0x00020000                          │  + 0x00020000
   │  (va_start - 0x48000000)               │  (same offset!)
   ▼                                        ▼
0x48020000 (va_start) ─────────────────► 0x30020000 (paddr)
```

---

#### `write_off` — Segment Offset Within `fake_flash`

```c
uint32_t write_off = seg.load_addr - 0x48000000;
memcpy(fake_flash + write_off, psram_buf + offset, seg.data_len);
```

`write_off` = how many bytes from the start of `fake_flash` this segment's data belongs.

Example: `load_addr = 0x48005000` → `write_off = 0x5000` (20,480 bytes from start)

```
psram_buf (packed):          fake_flash (sparse, at correct offsets):
┌─────────────┐              0x0000 ┌──────────────┐
│ seg_header  │                     │   [empty]    │
│ seg_data    │──────────────────►  │  seg_data    │ ← fake_flash + write_off
└─────────────┘              0x5000 └──────────────┘
```

Later when the MMU maps `fake_flash` → `0x48000000`, the CPU reading virtual `0x48005000` will hit exactly `fake_flash + 0x5000` — where you placed the segment data!

---

#### `page_start` and Image Header at Page Boundary

```c
uint32_t page_start = write_off & ~0xFFFF;
if (page_start != last_page_start) {
    memcpy(fake_flash + page_start, psram_buf, 32); // main image header at page start
    last_page_start = page_start;
}
```

- `page_start`: The 64 KB boundary that contains `write_off`
- The first 32 bytes copied from `psram_buf` is the **Main `esp_image_header_t`** (with the `0xE9` magic byte), **NOT** the segment header

> **Important distinction:** `psram_buf` starts with `esp_image_header_t` (the whole-image header, ~32 bytes). The `esp_image_segment_header_t` (8 bytes, just load_addr + data_len) is only used internally to calculate `write_off` and `data_len` and is **never written into `fake_flash`**.

```
                  64 KB Page inside fake_flash
page_start ──► ┌────────────────────────────────────────┐
(0x0000)       │ Main Image Header (esp_image_header_t) │ ← 32 bytes from psram_buf[0]
               ├────────────────────────────────────────┤
               │ Zeros / Padding                        │
write_off  ──► ┌────────────────────────────────────────┐
(e.g. 0x0020)  │ Segment Data Payload                   │ ← seg.data_len bytes
               └────────────────────────────────────────┘
```

`last_page_start` prevents re-copying the header if multiple segments fall in the same 64 KB page.

#### Why the Main Image Header at Each Page?

The ESP32 ROM bootloader / MMU verification routines check for the `0xE9` magic byte at **offset 0 of each mapped 64 KB page**. Placing the image header there satisfies those ROM/MMU checks.

#### Overlap Analysis

In standard ESP-IDF firmware, Espressif guarantees that `load_addr` within any page is at byte offset 32 (`0x20`) or higher, so the header (bytes 0–31) and segment data (byte 32+) **never overlap**.

For edge cases (custom/malicious binaries where `write_off < 32`), the bulletproof ordering is: **write header first, segment data second**. This way segment data always wins in any overlap, and the image header will be present as part of the segment payload at offset 0.

---

### PATH 2: Deferred Internal SRAM Copies

```c
safe_copies[safe_copy_count].dest = (void *)seg.load_addr;  // IRAM/DRAM address
safe_copies[safe_copy_count].src  = (void *)(psram_buf + offset);
safe_copies[safe_copy_count].len  = seg.data_len;
safe_copy_count++;
```

**Why deferred?** The current bootloader code is **running from Internal SRAM**. If you immediately `memcpy` into IRAM/DRAM right now, you would overwrite the currently executing code, stack, or FreeRTOS data — causing an instant crash. These copies are saved in `safe_copies[]` and performed by a trampoline routine right before the final jump.

---

### Final Cache Flush (After `fake_flash` is staged)

```c
Cache_WriteBack_Addr(0x10, (uint32_t)fake_flash, max_offset);  // D-Cache
Cache_WriteBack_Addr(0x20, (uint32_t)fake_flash, max_offset);  // I-Cache
```

The `memcpy` operations into `fake_flash` wrote through the D-Cache. The physical PSRAM may still have stale data. This flush ensures the MMU will expose fresh data when it maps `fake_flash` to `0x48000000`.

---

## 10. Execution Handshake

### Code
```c
safe_entry_addr = hdr.entry_addr;
for (int i = 0; i < mapping_count; i++) safe_mappings[i] = mmu_mappings[i];
safe_mapping_count = mapping_count;

ESP_LOGI(TAG, "Jumping to 0x%08lX ...", (unsigned long)safe_entry_addr);
for (int i = 0; i < safe_copy_count; i++)
    ESP_LOGI(TAG, "  copy[%d]: %p <- %p (%lu B)",
             i, safe_copies[i].dest, safe_copies[i].src, (unsigned long)safe_copies[i].len);

vTaskDelay(100 / portTICK_PERIOD_MS);
```

### Explanation

1. **Preserving Stack Variables into `safe_*` Globals:**  
   `hdr`, `mmu_mappings[]` are local (stack) variables. When the trampoline executes its final jump and tears down the stack, these would become invalid. Moving them into static/global `safe_*` variables ensures they survive.

2. **Log the Jump Plan:**  
   Prints the entry address and every deferred SRAM copy — lets you verify on serial that the correct firmware was loaded and will jump to the expected address.

3. **`vTaskDelay(100)`:**
   - Gives UART hardware **100 milliseconds to flush** serial log buffers to your terminal. Without this delay, the MMU reconfiguration might interrupt UART mid-transmission and cut off your last log lines.
   - Lets FreeRTOS tasks finish any pending operations before hardware state is altered.

---

## 11. Complete Handoff Sequence Summary

```
Step 1: esp_partition_read()
        Flash ──DMA──► psram_buf in PSRAM  OK

Step 2: Cache_WriteBack_Addr(psram_buf)
        Flush D-Cache + I-Cache for psram_buf

Step 3: Validate image header
        Magic 0xE9, segment_count 1-16, log entry_addr

Step 4: Walk segments → calculate max_offset
        How many bytes of PSRAM the firmware occupies (64KB aligned)

Step 5: heap_caps_aligned_alloc(fake_flash, max_offset)
        Allocate 64KB-aligned PSRAM target buffer

Step 6: esp_mmu_vaddr_to_paddr(fake_flash, &flash_paddr)
        Get physical address of fake_flash for MMU config

Step 7: Walk segments again → stage into fake_flash
        PSRAM segments:
          - Record mmu_mappings[]: vaddr, len, paddr
          - Place main image header at each new 64KB page_start
          - Copy segment payload to fake_flash + write_off
        IRAM/DRAM segments:
          - Record safe_copies[]: dest, src, len (deferred)

Step 8: Cache_WriteBack_Addr(fake_flash)
        Flush D-Cache + I-Cache for fake_flash

Step 9: Copy safe_* statics, log plan, vTaskDelay(100)

Step 10: Configure MMU
         mmu_mappings[]: Physical PSRAM → Virtual 0x48000000

Step 11: Execute safe_copies[] via trampoline
         Copy IRAM/DRAM segments into internal SRAM

Step 12: Jump to entry_addr → Target firmware runs!
```

---

## 12. `ESP_LOGI` Reference

```c
ESP_LOGI(TAG, format, ...args)
```

Output format:
```
I (1234) MY_TAG: Image OK: 3 segments, entry=0x4037C000
│  │      │       └─ formatted string (printf-style)
│  │      └─ TAG
│  └─ timestamp in ms since boot
└─ log level: I=Info, W=Warn, E=Error, D=Debug, V=Verbose
```

It is `printf` with a log level letter, millisecond timestamp, and TAG prepended. Not 4 separate printed values.
