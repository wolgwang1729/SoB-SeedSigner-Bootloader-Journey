// --- Standard library ---
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

// --- FreeRTOS ---
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// --- ESP-IDF: logging, heap, MMU ---
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "esp_cache.h"
#include "esp_rom_sys.h"
#include "esp_rom_uart.h"

// --- ESP-IDF: SD card (SDMMC1 native interface on ESP32-S3) ---
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

// --- ESP-IDF: flash payload partition (dev/bring-up fallback, Phase 11 style) ---
#include "esp_partition.h"
#include "esp_app_format.h"

// --- ESP-IDF: CPU / WDT / timer / cache registers ---
#include "esp_cpu.h"
#include "esp_intr_alloc.h"
#include "hal/cache_ll.h"
#include "hal/wdt_hal.h"
#include "xt_instr_macros.h"
#include "soc/timer_group_struct.h"
#include "soc/sensitive_reg.h"
#include "soc/timer_periph.h"
#include "soc/systimer_struct.h"
#include "soc/soc.h"
#include "soc/ext_mem_defs.h"

// --- Specter secure app loader (Phase_9 specter_crypto component) ---
#include "bl_section.h"
#include "bl_signature.h"
#include "anti_phish.h"

// ==================== ESP32-S3 Memory Map ====================
// The ESP32-S3 is an Xtensa LX7 dual-core SoC. Unlike the ESP32-P4, the S3
// shares ONE MMU table between the instruction and data windows (SRAM at
// DR_REG_MMU_TABLE, 512 entries, 64KB pages), so the I and D windows see the
// same "linear address" (vaddr & 0x1FFFFFF):
//
//   Internal SRAM (uncached):
//     IRAM 0x40370000 - 0x403E0000   (I/D alias; D-port can write 0x40374000+)
//     DRAM 0x3FC88000 - 0x3FD00000   (D alias of the same physical SRAM)
//   Flash cache windows (MMU):
//     IROM 0x42000000 - 0x44000000
//     DROM 0x3C000000 - 0x3E000000
//   PSRAM (this board: 8MB QSPI, physical 0x3D000000 - 0x3D800000)
//   MMU table (shared I/D): DR_REG_MMU_TABLE = 0x600C5000
//     entry_id = (vaddr & SOC_MMU_LINEAR_ADDR_MASK) >> 16
//     value    = (paddr >> 16) | SOC_MMU_ACCESS_SPIRAM   (BIT15 = PSRAM target)
//
// Key differences from the Phase 13/14 ESP32-P4 loader:
//   - No separate SPI_MEM_C / SPI_MEM_S register banks — a single SRAM table.
//   - ROM cache API takes (addr, size) — no leading "map" argument, and the
//     whole-cache Cache_Invalidate_ICache_All/DCache_All exist.
//   - Internal SRAM is uncached, so the P4's 64KB D-cache eviction/drain dance
//     (evict_buf) is unnecessary — the direct segment copies land in SRAM via
//     plain stores.
//   - Xtensa instead of RISC-V: no mie/mtvec/PMP; interrupts are masked with
//     `rsil` (PS.INTLEVEL = 15). The payload entry point resets the windowed
//     context itself (see hello_world_esp32s3_stock_shim/components/stateless_shim).
//   - The JMP zone is relocated to high IRAM (0x403A0000, loader_high.ld) so
//     the payload's IRAM copies (targeting 0x40374000) cannot clobber the
//     executing trampoline; the loader's own flash windows are remapped away
//     at JMP time, so the JMP zone must run from IRAM/ROM only.
// =============================================================

static const char *TAG = "SEEDSIGNER_LOADER";
#define MAX_FIRMWARE_SIZE (8 * 1024 * 1024)

// ---------------------------------------------------------------------------
// Specter vendor keys — the only keys authorized to sign the payload.
// This is the test key from Phase_9's generate_signed_payload.py (matches the
// C-printed `vendor_keys[]`; a real deployment replaces this with production
// keys held offline).
// ---------------------------------------------------------------------------
const bl_pubkey_t vendor_keys[] = {
    { .bytes = { 0x04, 0x08, 0xf4, 0xf3, 0x7e, 0x2d, 0x8f, 0x74, 0xe1, 0x8c, 0x1b, 0x8f, 0xde, 0x23, 0x74, 0xd5, 0xf2, 0x84, 0x02, 0xfb, 0x8a, 0xb7, 0xfd, 0x1c, 0xc5, 0xb7, 0x86, 0xaa, 0x40, 0x85, 0x1a, 0x70, 0xcb, 0xc2, 0xec, 0xa8, 0x7b, 0x8b, 0xd2, 0xc0, 0xbe, 0x52, 0x69, 0x8e, 0x9d, 0x5e, 0xe1, 0x98, 0x40, 0xc4, 0xd4, 0x0c, 0xa6, 0x96, 0xe1, 0x61, 0x59, 0x13, 0x47, 0x69, 0xfa, 0x1a, 0xe8, 0x5b, 0x2e } },
    BL_PUBKEY_END_OF_LIST
};
const bl_pubkey_t *pubkeys_boot[] = { vendor_keys, NULL };

// Minimum number of valid signatures required before the loader boots a
// payload. This deployment authorizes exactly one vendor key (single-key
// multisig), so at least 1 valid signature is required. blsig_verify_multisig()
// returns the *count* of valid signatures (0 = none recognized) and only uses
// negative values for errors, so the caller MUST enforce a lower bound itself —
// blsig_is_error() alone would accept 0 as success (upstream Specter enforces
// this per-keyset threshold in bootloader.c verify_multisig(); the loader has
// to replicate it here).
#define SIG_THRESHOLD 1

// Progress callback fed to blsig_verify_multisig (secp256k1 is slow). We yield
// instead of esp_task_wdt_reset() — the main task isn't auto-subscribed to the
// TWDT in IDF v5.
static void crypto_progress_cb(void *ctx, bl_cbarg_t arg, uint32_t total, uint32_t complete)
{
    (void)ctx;
    (void)arg;
    (void)total;
    (void)complete;
    vTaskDelay(1);
}

#define MOUNT_POINT "/sdcard"
#define SD_FIRMWARE_PATH MOUNT_POINT "/seedsigner_esp32s3.bin"

// Mount the SD card via the native SDMMC1 peripheral (ESP32-S3).
//   Bus:    4-bit, explicit pins (CLK=36, CMD=35, D0=37, D1=38, D2=33, D3=34)
//   Power:  external 3.3V (the S3 has no on-chip SD LDO; DevKitC-1 compatible)
// The same wiring is used by the MicroPython payload's board init, so one card
// config covers both the loader read and the runtime firmware.
sdmmc_card_t *mount_storage_sdcard(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk   = 36;
    slot_config.cmd   = 35;
    slot_config.d0    = 37;
    slot_config.d1    = 38;
    slot_config.d2    = 33;
    slot_config.d3    = 34;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (%s).", esp_err_to_name(ret));
        return NULL;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return card;
}

// ---------------------------------------------------------------------------
// Dev/bring-up flash payload path (Phase 11 style).
//
// With no SD card attached (the S3 dev kit's SD wiring is not soldered yet),
// the loader boots a RAW 0xE9 ESP32 image flashed into the 'payload' data
// partition. This path deliberately SKIPS the Specter bundle verification —
// it exists purely to bring up the loader -> JMP -> PSRAM execution chain on
// silicon before the SD hardware exists. It is NOT the production path; the
// production flow always reads /sdcard/seedsigner_esp32s3.bin.
// ---------------------------------------------------------------------------
// DIAGNOSTIC: forward decl — dump_cache_err_status is defined after
// load_flash_payload but used inside it. TODO remove.
void dump_cache_err_status(const char *where);

// DIAGNOSTIC: print CPU PS/window state. TODO remove.
static void diag_ps(const char *where)
{
    uint32_t _ps, _wb, _ws, _ai;
    asm volatile ("rsr %0, ps" : "=r"(_ps));
    asm volatile ("rsr %0, windowbase" : "=r"(_wb));
    asm volatile ("rsr %0, windowstart" : "=r"(_ws));
    asm volatile ("rsr %0, intenable" : "=r"(_ai));
    ESP_LOGI(TAG, "[DBG] ps @%s: ps=0x%08X wb=%d ws=0x%08X intena=0x%08X",
             where, _ps, _wb, _ws, _ai);
}

uint8_t *load_flash_payload(size_t *fw_size_out)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0xFF, "payload");
    if (!part) {
        ESP_LOGE(TAG, "'payload' partition not found. Halting.");
        return NULL;
    }

    esp_image_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK || hdr.magic != 0xE9) {
        ESP_LOGE(TAG, "'payload' partition empty or invalid magic (0x%02X). Halting.", hdr.magic);
        return NULL;
    }

    // Compute actual firmware image size from segment headers
    size_t fw_size = sizeof(esp_image_header_t);
    for (int i = 0; i < hdr.segment_count && i < 16; i++) {
        esp_image_segment_header_t seg;
        if (esp_partition_read(part, fw_size, &seg, sizeof(seg)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read segment %d header", i);
            return NULL;
        }
        fw_size += sizeof(seg) + seg.data_len;
    }
    // Add 16 bytes for padding/checksum
    fw_size = (fw_size + 15) & ~15;
    if (fw_size == 0 || fw_size > MAX_FIRMWARE_SIZE || fw_size > part->size) {
        ESP_LOGE(TAG, "Invalid flash payload size: %lu. Halting.", (unsigned long)fw_size);
        return NULL;
    }

    uint8_t *psram_buf = heap_caps_aligned_alloc(65536, fw_size, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "[DBG] PSRAM alloc done: %p (%lu bytes)", psram_buf, (unsigned long)fw_size);
    if (!psram_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%lu bytes). Halting.", (unsigned long)fw_size);
        return NULL;
    }

    if (esp_partition_read(part, 0, psram_buf, fw_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read 'payload' partition. Halting.");
        free(psram_buf);
        return NULL;
    }

    *fw_size_out = fw_size;
    ESP_LOGI(TAG, "[FLASH PARTITION] Loaded %lu bytes from 'payload' @ 0x%08lX "
                  "(dev path, no Specter verification)",
             (unsigned long)fw_size, (unsigned long)part->address);
    return psram_buf;
}

// DIAGNOSTIC: dump the cache error status registers (S3). TODO remove.
void dump_cache_err_status(const char *where)
{    uint32_t ilg = cache_ll_l1_get_illegal_error_intr_status(0, CACHE_LL_L1_ILG_EVENT_MASK);
    uint32_t acs0 = cache_ll_l1_get_access_error_intr_status(0, CACHE_LL_L1_ACCESS_EVENT_MASK);
    uint32_t acs1 = cache_ll_l1_get_access_error_intr_status(1, CACHE_LL_L1_ACCESS_EVENT_MASK);
    uint32_t dbus_reject = 0;
    if (acs0 & CACHE_LL_L1_ACCESS_EVENT_DBUS_REJECT) {
        dbus_reject = cache_ll_get_acs_dbus_reject_vaddr(0);
    } else if (acs1 & CACHE_LL_L1_ACCESS_EVENT_DBUS_REJECT) {
        dbus_reject = cache_ll_get_acs_dbus_reject_vaddr(1);
    }
    ESP_LOGI(TAG, "[DBG] cache-err status @%s: ilg=0x%08X acs0=0x%08X acs1=0x%08X dbus_reject_vaddr=0x%08X",
             where, ilg, acs0, acs1, dbus_reject);
}


// ---------------------------------------------------------------------------
// JMP zone — relocated to high IRAM by loader_high.ld
// ---------------------------------------------------------------------------
// Everything the loader needs between "point of no return" and the payload
// entry lives in the `.jmp_zone` region (code @ 0x403A0000, data right after).
// This is critical on the S3: the loader's stock IRAM (0x40374000+) is the
// payload's IRAM copy target, and the loader's flash windows (0x42000000 /
// 0x3C000000) are remapped to PSRAM at JMP time — so after the MMU remap only
// ROM and `.jmp_zone` (internal SRAM) code may execute.
#define JMP_ZONE_TEXT __attribute__((section(".jmp_zone.text")))
#define JMP_ZONE_BSS  __attribute__((section(".jmp_zone.bss")))
#define JMP_ZONE_STACK __attribute__((section(".jmp_zone.stack")))

// The payload's IRAM segments must stay below this (checked at staging time),
// otherwise the deferred copies would clobber the running JMP zone.
#define JMP_ZONE_BASE 0x403A0000
#define JMP_ZONE_END  0x403B8000

typedef struct { void *dest; void *src; uint32_t len; } pending_copy_t;
typedef struct { uint32_t vaddr; uint32_t paddr; uint32_t len; } mmu_mapping_t;

JMP_ZONE_BSS static pending_copy_t safe_copies[20];
JMP_ZONE_BSS static int            safe_copy_count;
JMP_ZONE_BSS static uint32_t       safe_entry_addr;
JMP_ZONE_BSS static mmu_mapping_t  safe_mappings[20];
JMP_ZONE_BSS static int            safe_mapping_count;
JMP_ZONE_BSS static uint32_t       diag_a2_val;    /* TODO remove */
JMP_ZONE_BSS static uint32_t       diag_a10_val;   /* TODO remove */

// DIAG TODO remove. diag_jmp_progress lived at 0x3FC9C010 == the stack top
// (SP), so every esp_rom_printf windowed call overwrote it with the callee's
// return address -- leftover readouts were unreliable. Keep it at a fixed
// address that is (a) outside every payload copy range (0x3FC88000-0x3FC985C4),
// (b) above the zone stack top (0x3FC9C010) so window overflow/underflow save
// areas never touch it, and (c) below the internal heap (_heap_start 0x3FCC8000)
// and _jmp_zone_bss_end (<=0x3FCB0000). 0x3FCA0000 is all three.
#define DIAG_MARKER  0x3FCA0000
#define DIAG_STAGE   0x3FCA8000
// Robust progress store: the address is materialized INSIDE the asm, right
// before the store, so no register holding it can be hoisted across a call and
// clobbered by a ROM callee (observed: DIAG_MARKER kept in a7 across callx8
// esp_rom_printf -> a7=0 -> StoreProhibited on the progress store).
#define DIAG_PROGRESS(x) do {                                             \
    uint32_t _diag_a;                                                     \
    asm volatile ("movi %0, %1\n\t" "s32i.n %2, %0, 0\n\t"                \
                  : "=&r"(_diag_a)                                        \
                  : "i"(DIAG_MARKER), "r"((uint32_t)(x)) : "memory");     \
} while (0)
#define DIAG_READ()       (*(volatile uint32_t *)DIAG_MARKER)
#define diag_jmp_progress DIAG_READ()

// Dedicated stack for the JMP zone and the payload's early boot. The default
// main-task stack is carved from the internal DRAM heap (0x3FC8xxxx) and lands
// inside the payload's DRAM copy region (0x3fc90600+) — its frames would be
// clobbered by the deferred copies. jump_stack lives in `.jmp_zone` (high IRAM,
// clear of the payload), so the JMP zone and the payload's early boot run on it
// and never touch the abandoned main-task stack.
//
// `.jmp_zone.stack` is placed FIRST in the `.jmp_zone.bss` output section (low
// addresses), and the zone's state vars (`.jmp_zone.bss`) AFTER a gap, so the
// stack grows down from a top that is far BELOW the vars. This matters: the
// Xtensa windowed overflow handler saves wrapped windows at save areas that
// extend UPWARD from the top-of-stack, and any printf call from the zone (or
// the payload's own calls) would otherwise clobber the vars that sit right
// above the stack top. See loader_high.ld for the padding.
JMP_ZONE_STACK static uint8_t jump_stack[16384] __attribute__((aligned(16)));

// ---------------------------------------------------------------------------
// ROM-safe helpers (stack strings only; callable with flash remapped away)
// ---------------------------------------------------------------------------
static void JMP_ZONE_TEXT bootloader_uart0_print(const char *str)
{
    if (!str) return;

    volatile uint32_t *uart0_fifo   = (volatile uint32_t *)0x60000000;
    volatile uint32_t *uart0_status = (volatile uint32_t *)0x6000001C;

    while (*str) {
        char ch = *str++;

        // Hardware UART0
        uint32_t status = *uart0_status;
        if (status == 0xFFFFFFFF) {
            *uart0_fifo = (uint32_t)ch;
        } else {
            volatile uint32_t timeout = 100000;
            while ((((status = *uart0_status) >> 16) & 0xFF) >= 126 && --timeout > 0) {
                if (status == 0xFFFFFFFF) break;
            }
            *uart0_fifo = (uint32_t)ch;
        }
    }
}

static void JMP_ZONE_TEXT dbg_print_hex(uint32_t val)
{
    char hex[]    = "0x00000000";
    char digits[] = "0123456789ABCDEF";
    for (int i = 9; i >= 2; i--) {
        hex[i] = digits[val & 0xF];
        val >>= 4;
    }
    bootloader_uart0_print(hex);
}

// ---------------------------------------------------------------------------
// do_mmu_mapping_and_jump — point of no return
// ---------------------------------------------------------------------------
// Relax the S3 PMS memory protection for the JMP handoff. ESP-IDF arms the
// S3's memprot feature at startup (CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=y) with
// the IRAM0 split line pinned to `_iram_text_end` and the areas set to:
//   IRAM0_0/1/2 (below split): READ|EXEC  -> D-writes silently dropped
//   IRAM0_3     (above split): NONE       -> I-fetch + D-reads return 0
// The JMP zone (0x403A0000, above the split) is therefore neither executable
// nor accessible until the areas are relaxed, and the deferred payload copies
// into IRAM0 (0x40374000+) need WRITE. This sets every IRAM0 WORLD_0 area to
// READ|WRITE|EXEC (both cores; world 1 registers set identically for good
// measure). Requires CONFIG_ESP_SYSTEM_MEMPROT_FEATURE_LOCK=n, else the
// SENSITIVE registers are hardware-locked and these writes are ignored.
// Runs from normal (flash-resident) loader context BEFORE the JMP zone entry.
static void jmp_prepare(void)
{
    // DIAG (temporary): WDTs LEFT ENABLED so a fault in the JMP zone resets with
    // a ROM "Saved PC" that pinpoints where the CPU got stuck. TODO: re-enable
    // the disables once the zone entry hang is fixed.
    esp_rom_printf("JPB\n");
}
// DIAGNOSTIC: naked `ret.n` probe — proves whether the JMP-zone region is
// executable (I-fetch works) at all. TODO remove.
void JMP_ZONE_TEXT __attribute__((naked, noinline)) jmp_zone_probe(void)
{
    // Must be call8/retw.n: the compiler emits `callx8` for this direct call,
    // so a bare `ret.n` returns with the window still rotated by 8 and the
    // caller resumes from a bit-31-set return address -> InstrFetchProhibited.
    __asm__ __volatile__("retw.n");
}

// DIAG: multi-instruction naked zone function (no entry, no calls, no l32r) —
// builds the UART0 FIFO address with ALU ops and writes 'N'. TODO remove.
void JMP_ZONE_TEXT __attribute__((naked, noinline)) zone_naked_test(void)
{
    __asm__ __volatile__(
        "   movi a2, 0x6000\n"
        "   slli a2, a2, 16\n"
        "   movi a3, 0x4E\n"
        "   s32i a3, a2, 0\n"
        "   retw.n\n");
}

// DIAG: pure-instruction zone test (no .word literals, no alignment pads).
// Probes l8ui-from-IRAM0 / l32i-from-MMIO / s32i-to-MMIO, writing a progress
// letter to the UART FIFO after each step. TODO remove.
void JMP_ZONE_TEXT __attribute__((naked, noinline)) zone_l32r_test(void)
{
    __asm__ __volatile__(
        "   movi a2, 0x403A\n"      /* 0x403A0000 = zone base (IRAM0, D-readable) */
        "   slli a2, a2, 16\n"
        "   movi a3, 0x6000\n"      /* 0x60000000 = UART0 FIFO (ESP32-S3) */
        "   slli a3, a3, 16\n"
        "   l32i a4, a2, 0\n"       /* word load from IRAM0 (byte l8ui would LoadStoreError) */
        "   movi a5, 0x41\n"        /* 'A' */
        "   s32i a5, a3, 0\n"
        "   movi a5, 0x42\n"        /* 'B' */
        "   s32i a5, a3, 0\n"
        "   l32i a4, a3, 0x1C\n"    /* l32i from UART0 status (MMIO) */
        "   movi a5, 0x43\n"        /* 'C' */
        "   s32i a5, a3, 0\n"
        "   s32i a4, a3, 0\n"       /* s32i to UART0 FIFO (MMIO write) */
        "   movi a5, 0x44\n"        /* 'D' */
        "   s32i a5, a3, 0\n"
        "   retw.n\n");
}

// DIAG: does a `l8ui` byte read from DROM (flash rodata) work from inside the
// zone? Arg (string ptr) arrives in callee-a2 via callx8 (caller a10). Writes
// '1' after the l8ui, '2' after memw. TODO remove.
void JMP_ZONE_TEXT __attribute__((naked, noinline)) zone_byte_read_test(const char *s)
{
    __asm__ __volatile__(
        "   movi a3, 0x6000\n"
        "   slli a3, a3, 16\n"
        "   l8ui a4, a2, 0\n"       /* byte read from DROM */
        "   movi a5, 0x31\n"        /* '1' */
        "   s32i a5, a3, 0\n"
        "   memw\n"
        "   movi a5, 0x32\n"        /* '2' */
        "   s32i a5, a3, 0\n"
        "   retw.n\n");
}

// DIAG: replicates bootloader_uart0_print's per-char loop core with markers:
// 'A' after status-read+bnei, 'B' after stack write/read, 'C' after the
// timeout-decrement loop. All ops word-width; no `.word` literals. TODO remove.
void JMP_ZONE_TEXT __attribute__((naked, noinline)) zone_loop_core_test(void)
{
    __asm__ __volatile__(
        "   movi a2, 0x6000\n"
        "   slli a2, a2, 16\n"
        "   movi a3, 0x6000\n"
        "   slli a3, a3, 16\n"
        "   addi a3, a3, 0x1C\n"           /* a3 = status 0x6000001C */
        "   l32i a4, a3, 0\n"              /* status read */
        "   movi a5, 0x41\n"               /* 'A' */
        "   s32i a5, a2, 0\n"
        "   bnei a4, -1, 1f\n"
        "   movi a5, 0x21\n"               /* '!' (status==-1 path) */
        "   s32i a5, a2, 0\n"
        "1:\n"
        "   movi a6, 100000\n"             /* timeout to stack */
        "   s32i a6, a1, 0\n"
        "   l32i a6, a1, 0\n"
        "   movi a5, 0x42\n"               /* 'B' */
        "   s32i a5, a2, 0\n"
        "   movi a7, 100000\n"
        "   movi a15, 1\n"
        "2:\n"
        "   l32i a9, a1, 0\n"
        "   addi.n a9, a9, -1\n"
        "   nsau a8, a9\n"
        "   s32i a9, a1, 0\n"
        "   srli a8, a8, 5\n"
        "   beqz a9, 3f\n"
        "   addi.n a10, a10, 1\n"
        "   moveqz a8, a15, a10\n"
        "   beqz a8, 2b\n"
        "3:\n"
        "   movi a5, 0x43\n"               /* 'C' */
        "   s32i a5, a2, 0\n"
        "   retw.n\n");
}

// DIAG: calls the REAL bootloader_uart0_print from inside the zone via call8,
// exactly as do_mmu_mapping_and_jump does. Moves the arg a2→a10 (call8 window:
// callee reads caller's a10 as its a2). TODO remove.
// DIAG: records a2/a10 into RAM, prints *a2 and *a10 as hex digits, then calls
// the real bootloader_uart0_print via call8 (as do_mmu_mapping_and_jump does).
// TODO remove.
#define ZWRAP_HEXBYTE(reg)                       \
    "   l8ui a5, " #reg ", 0\n"                  \
    "   srli a7, a5, 4\n"                        \
    "   extui a7, a7, 0, 4\n"                    \
    "   addi a7, a7, 0x30\n"                     \
    "   s32i a7, a6, 0\n"                        \
    "   extui a7, a5, 0, 4\n"                    \
    "   addi a7, a7, 0x30\n"                     \
    "   s32i a7, a6, 0\n"
void JMP_ZONE_TEXT __attribute__((naked, noinline)) zone_wrap_print_test(const char *s)
{
    __asm__ __volatile__(
        "   movi a4, diag_a2_val\n"
        "   s32i a2, a4, 0\n"           /* record a2 */
        "   movi a4, diag_a10_val\n"
        "   s32i a10, a4, 0\n"          /* record a10 */
        "   movi a6, 0x6001\n"
        "   slli a6, a6, 16\n"
        "   addmi a6, a6, 0x3000\n"     /* a6 = FIFO */
        ZWRAP_HEXBYTE(a2)
        "   mov a10, a2\n"
        ZWRAP_HEXBYTE(a10)
        "   call8 bootloader_uart0_print\n"
        "   retw.n\n");
}
#undef ZWRAP_HEXBYTE

// DIAG: does the compiler-emitted `entry` + a call-out-of-zone work? The entry
// is emitted by the compiler at the start of every naked function (verified in
// the disasm of the other tests) — an explicit `entry` here would rotate the
// register window a SECOND time and clobber a2/a3, so we rely on the implicit
// one. Writes 'E' then returns. TODO remove.
void JMP_ZONE_TEXT __attribute__((naked, noinline)) zone_entry_test(void)
{
    __asm__ __volatile__(
        "   movi a2, 0x6000\n"
        "   slli a2, a2, 16\n"
        "   movi a3, 0x45\n"
        "   s32i a3, a2, 0\n"
        "   retw.n\n");
}
// Runs entirely from `.jmp_zone` IRAM (internal SRAM, uncached). Ordering is
// load-bearing:
//   1. mask all interrupts (rsil 15) and switch SP to jump_stack — the main
//      task stack may be clobbered by the payload copies from here on,
//   2. disable RWDT/MWDT0/MWDT1 while flash is still mapped (wdt_hal code is
//      in the loader's flash .text),
//   3. silence SYSTIMER/timer-group interrupts,
//   4. write back the PSRAM staging buffers (Cache_WriteBack_Addr) so the
//      payload's reads through the freshly-remapped windows see the bytes that
//      were written through the old 0x3D000000 window,
//   5. copy the direct segments into internal SRAM (uncached — plain stores),
//   6. program the shared MMU table (DR_REG_MMU_TABLE) so the IROM/DROM windows
//      point at the PSRAM-backed fake_flash pages,
//   7. invalidate both caches (ROM),
//   8. drain the UART TX FIFO and jump to the payload entry.
void JMP_ZONE_TEXT __attribute__((noreturn)) do_mmu_mapping_and_jump(void)
{
    // Mask all interrupts. The stack switch below abandons the FreeRTOS main
    // task context, so no ISR may fire between here and the payload handoff.
    asm volatile ("rsil a2, 15" ::: "memory");

    uint32_t sp_top = (uint32_t)(jump_stack + sizeof(jump_stack)) - SAVE_AREA_OFFSET;
    SET_STACK(sp_top);

    // Clear FreeRTOS hardware watchpoints
    esp_cpu_clear_watchpoint(0);
    esp_cpu_clear_watchpoint(1);

    // Disable the watchdog timers: the payload re-initializes them itself once
    // it boots, but between here and there the boot path must not be reset.
    {
        wdt_hal_context_t rwdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
        wdt_hal_write_protect_disable(&rwdt_ctx);
        wdt_hal_disable(&rwdt_ctx);
        wdt_hal_write_protect_enable(&rwdt_ctx);
    }

    wdt_hal_context_t mwdt0_ctx = {.inst = WDT_MWDT0, .mwdt_dev = &TIMERG0};
    wdt_hal_write_protect_disable(&mwdt0_ctx);
    wdt_hal_disable(&mwdt0_ctx);
    wdt_hal_write_protect_enable(&mwdt0_ctx);

    wdt_hal_context_t mwdt1_ctx = {.inst = WDT_MWDT1, .mwdt_dev = &TIMERG1};
    wdt_hal_write_protect_disable(&mwdt1_ctx);
    wdt_hal_disable(&mwdt1_ctx);
    wdt_hal_write_protect_enable(&mwdt1_ctx);

    // Silence SYSTIMER and timer-group interrupts
    SYSTIMER.int_ena.val = 0;
    SYSTIMER.int_clr.val = 0xFFFFFFFF;
    TIMERG0.int_ena_timers.val = 0;
    TIMERG0.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG1.int_ena_timers.val = 0;
    TIMERG1.int_clr_timers.val = 0xFFFFFFFF;

    // Write back the PSRAM staging buffers so the payload sees the bytes when
    // it reads the remapped windows (D-cache is write-back on the S3).
    if (safe_mapping_count > 0) {
        extern int Cache_WriteBack_Addr(uint32_t vaddr, uint32_t size);
        extern uint8_t *fake_flash_ptr;
        extern uint32_t fake_flash_len;
        Cache_WriteBack_Addr((uint32_t)fake_flash_ptr, fake_flash_len);
    }

    // Copy direct segments into internal SRAM and RTC RAM.
    // IRAM0 space and RTC RAM spaces must use 32-bit word stores (s32i).
    for (int i = 0; i < safe_copy_count; i++) {
        uint32_t dest_addr = (uint32_t)safe_copies[i].dest;
        const uint8_t *src = (const uint8_t *)safe_copies[i].src;
        uint32_t len = safe_copies[i].len;

        if ((dest_addr >= 0x40370000 && dest_addr < 0x403E0000) ||
            (dest_addr >= 0x50000000 && dest_addr < 0x50020000) ||
            (dest_addr >= 0x600FE000 && dest_addr < 0x60100000)) {
            volatile uint32_t *d32 = (volatile uint32_t *)dest_addr;
            for (uint32_t j = 0; j < len; j += 4) {
                uint32_t word = 0;
                uint32_t remain = len - j;
                if (remain >= 4) {
                    word = (uint32_t)src[j] |
                           ((uint32_t)src[j+1] << 8) |
                           ((uint32_t)src[j+2] << 16) |
                           ((uint32_t)src[j+3] << 24);
                } else {
                    for (uint32_t k = 0; k < remain; k++) {
                        word |= ((uint32_t)src[j+k] << (k * 8));
                    }
                }
                d32[j / 4] = word;
            }
        } else {
            uint8_t *d = (uint8_t *)dest_addr;
            for (uint32_t j = 0; j < len; j++) d[j] = src[j];
        }
    }

    // Program the shared I/D MMU table so the IROM/DROM windows point at the
    // PSRAM-backed fake_flash pages.
    for (int i = 0; i < safe_mapping_count; i++) {
        uint32_t first_entry = (safe_mappings[i].vaddr & SOC_MMU_LINEAR_ADDR_MASK) >> 16;
        uint32_t paddr_page  = safe_mappings[i].paddr >> 16;
        uint32_t pages       = (safe_mappings[i].len + 0xFFFF) / 0x10000;

        for (uint32_t e = 0; e < pages; e++) {
            *(volatile uint32_t *)(DR_REG_MMU_TABLE + (first_entry + e) * 4) =
                (paddr_page + e) | SOC_MMU_ACCESS_SPIRAM;
        }
    }

    // Invalidate both caches so the freshly-remapped windows are fetched fresh.
    extern void Cache_Invalidate_ICache_All(void);
    extern void Cache_Invalidate_DCache_All(void);
    Cache_Invalidate_ICache_All();
    Cache_Invalidate_DCache_All();
    asm volatile ("isync" ::: "memory");

    // Drain UART TX FIFO before jumping
    volatile uint32_t *uart_status = (volatile uint32_t *)0x6000001C;
    volatile uint32_t drain_timeout = 100000;
    while (((*uart_status >> 16) & 0xFF) > 0 && --drain_timeout > 0) {}

    typedef void (*entry_t)(void) __attribute__((noreturn));
    ((entry_t)safe_entry_addr)();

    while (1);
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
void app_main(void)
{
    // DIAGNOSTIC: keep the spurious cache-error interrupt from panicking so we
    // can observe the flash-op path. TODO remove.
    esp_cpu_intr_disable(1 << ETS_CACHEERR_INUM);
    diag_ps("app_main entry");
    dump_cache_err_status("app_main entry");

    // DIAGNOSTIC: are JMP-zone BSS stores visible? TODO remove.
    safe_entry_addr = 0xDEADBEEF;
    safe_copy_count = 0x12345678;
    ESP_LOGI(TAG, "[DBG] jmpzone write test: &entry=0x%08lX entry=0x%08lX copies=%d",
             (unsigned long)&safe_entry_addr,
             (unsigned long)safe_entry_addr, safe_copy_count);
    ESP_LOGI(TAG, "[DBG] jmp_progress leftover = %lu", (unsigned long)diag_jmp_progress);
    safe_entry_addr = 0;
    safe_copy_count = 0;
    DIAG_PROGRESS(0);
    // DIAGNOSTIC: which transport carries the console? UART0 (0x60000000) vs
    // USB-SERIAL-JTAG (0x60038000): write a distinctive byte to each.
    // TODO remove.
    {
        volatile uint32_t *uart_fifo = (volatile uint32_t *)0x60000000;
        *uart_fifo = 0x55; /* 'U' */
        for (int i = 0; i < 20000; i++) __asm__ __volatile__("nop");
        volatile uint32_t *usb_ep1_conf = (volatile uint32_t *)0x60038004;
        volatile uint32_t *usb_ep1      = (volatile uint32_t *)0x60038000;
        uint32_t tries = 0;
        while (!(*usb_ep1_conf & BIT(1)) && tries++ < 100000) {}
        *usb_ep1 = 0x4A; /* 'J' */
        *usb_ep1_conf |= BIT(0); /* WR_DONE */
        for (int i = 0; i < 20000; i++) __asm__ __volatile__("nop");
    }
    ESP_LOGI(TAG, "[DBG] console probe done");
    // DIAGNOSTIC: memprot/PMS state + zone residency. With the memprot feature
    // DISABLED the IRAM0 constrains stay at reset default (0x1FFFFF = all R|W|X),
    // so the JMP zone (0x403A0000) should be fully accessible here. TODO remove.
    {
        ESP_LOGI(TAG, "[DBG] jmp_zone_probe @%p", (void *)jmp_zone_probe);
        // DIAG: dump the current (default) shared MMU table so we can see the
        // exact entry format for PSRAM-backed pages. TODO remove.
        {
            const volatile uint32_t *mmu = (const volatile uint32_t *)DR_REG_MMU_TABLE;
            for (int i = 0; i < 16; i += 4) {
                ESP_LOGI(TAG, "[DBG] mmu[%d..%d] = %08X %08X %08X %08X",
                         i, i + 3, mmu[i], mmu[i + 1], mmu[i + 2], mmu[i + 3]);
            }
            const volatile uint32_t *mmu_ps = (const volatile uint32_t *)(DR_REG_MMU_TABLE + 4 * 0x20);
            ESP_LOGI(TAG, "[DBG] mmu[0x20..0x23] = %08X %08X %08X %08X",
                     mmu_ps[0], mmu_ps[1], mmu_ps[2], mmu_ps[3]);
            // DIAG: does a bare MMU-table write stick in normal (cache-on)
            // context? Entry 16, then restore. TODO remove.
            volatile uint32_t *mmu16 = (volatile uint32_t *)(DR_REG_MMU_TABLE + 4 * 16);
            uint32_t saved16 = *mmu16;
            *mmu16 = 0x7F00;
            uint32_t rb16 = *mmu16;
            *mmu16 = saved16;
            ESP_LOGI(TAG, "[DBG] mmu16 write+readback: wrote 7F00 read %08X (was %08X)",
                     rb16, saved16);
        }
        ESP_LOGI(TAG, "[DBG] IRAM0 world0 PMS constrain reg 2 = 0x%08X (expect 0x1FFFFF: all R|W|X)",
                 (unsigned)REG_READ(SENSITIVE_CORE_X_IRAM0_PMS_CONSTRAIN_2_REG));
        // DRAM alias of the JMP-zone TEXT (0x403A0000 -> 0x3FCB0000): proves the
        // bootloader's segment copy landed and wasn't clobbered by loader data.
        const volatile uint32_t *zt_ali = (const volatile uint32_t *)0x3FCB0000;
        ESP_LOGI(TAG, "[DBG] dram alias of jmpzone text @0x3FCB0000: %08X %08X %08X %08X",
                 zt_ali[0], zt_ali[1], zt_ali[2], zt_ali[3]);
        // data-read the loader's OWN running IRAM text
        const volatile uint32_t *zown = (const volatile uint32_t *)0x40374000;
        ESP_LOGI(TAG, "[DBG] own IRAM0 data read @0x40374000: %08X %08X", zown[0], zown[1]);
        // IRAM0 D-read/write of the zone region itself — with memprot off these
        // must return the resident code / stick, respectively.
        {
            const volatile uint32_t *zt = (const volatile uint32_t *)0x403A0000;
            ESP_LOGI(TAG, "[DBG] ir0 read @0x403A0000: %08X %08X", zt[0], zt[1]);
            volatile uint32_t *q = (volatile uint32_t *)0x403A1000;
            q[0] = 0xCAFE0000;
            ESP_LOGI(TAG, "[DBG] ir0 write/read 0x403A1000 -> %08X", q[0]);
        }
    }
    ESP_LOGI(TAG, "SeedSigner Loader — ESP32-S3 PSRAM payload");
    dump_cache_err_status("after banner");

    // DIAGNOSTIC control: does a cache-suspending flash read work BEFORE the
    // SD mount attempt? If this hangs, flash ops are broken in the loader
    // regardless of SD. If it works but the later read hangs, the SD mount is
    // the trigger. TODO remove.
    {
        const esp_partition_t *dpart = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, 0xFF, "payload");
        if (dpart) {
            uint8_t m = 0;
            esp_err_t e = esp_partition_read(dpart, 0, &m, 1);
            ESP_LOGI(TAG, "[DBG] pre-SD flash read: rc=0x%x magic=0x%02X", e, m);
            dump_cache_err_status("after pre-SD flash read");
            diag_ps("after pre-SD flash read");
        } else {
            ESP_LOGI(TAG, "[DBG] pre-SD control: 'payload' partition not found");
        }
    }

    uint8_t *psram_buf = NULL;
    size_t   fw_size   = 0;

    // Explicitly initialize the JMP-zone state (the `.jmp_zone.bss` region is
    // not covered by the loader's normal BSS clear).
    // DIAGNOSTIC: JMP-zone BSS stores temporarily disabled to isolate a
    // cache-error panic at app_main entry. TODO revert.
    // safe_copy_count = 0;
    // safe_mapping_count = 0;

    // ----------------------------------------------------------------
    // Step 1: Load the payload.
    //   Production path  : /sdcard/seedsigner_esp32s3.bin — a Specter bundle
    //                      [bl_section_t "main"][raw ESP32 image][bl_section_t
    //                      "sign"], verified below. SD card unmounted right
    //                      after the read (TOCTOU-safe).
    //   Dev/bring-up path: no SD card -> raw 0xE9 image from the flash
    //                      'payload' partition (skips Specter verification).
    // ----------------------------------------------------------------
    bool flash_payload = false;
    // DIAGNOSTIC: SD mount disabled to verify the octal-PSRAM pin collision
    // theory (SDMMC on GPIO 33-38 overlaps MSPI octal PSRAM D4-D7/DQS).
    // TODO: re-enable + move SD pins to non-MSPI GPIOs before production SD path.
    sdmmc_card_t *card = NULL;
    // sdmmc_card_t *card = mount_storage_sdcard();
    dump_cache_err_status("after SD mount attempt");
    if (card == NULL) {
        ESP_LOGW(TAG, "No SD card — falling back to the flash 'payload' partition "
                      "(dev/bring-up path).");
        flash_payload = true;
        psram_buf = load_flash_payload(&fw_size);
        if (!psram_buf) {
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    } else {

    struct stat st;
    if (stat(SD_FIRMWARE_PATH, &st) != 0) {
        ESP_LOGE(TAG, "Firmware file %s not found. Halting.", SD_FIRMWARE_PATH);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    if (st.st_size == 0 || st.st_size > MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Invalid firmware size: %ld. Halting.", (long)st.st_size);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    fw_size = st.st_size;

    psram_buf = heap_caps_aligned_alloc(65536, fw_size, MALLOC_CAP_SPIRAM);
    if (!psram_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%lu bytes). Halting.", (unsigned long)fw_size);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    FILE *f = fopen(SD_FIRMWARE_PATH, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s. Halting.", SD_FIRMWARE_PATH);
        free(psram_buf);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    if (fread(psram_buf, 1, fw_size, f) != fw_size) {
        ESP_LOGE(TAG, "Failed to read firmware file entirely. Halting.");
        fclose(f);
        free(psram_buf);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    fclose(f);

    ESP_LOGI(TAG, "Unmounting SD card before verification (TOCTOU-safe)...");
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    ESP_LOGI(TAG, "[SD CARD] Loaded %lu bytes from %s", (unsigned long)fw_size,
             SD_FIRMWARE_PATH);
    } // end else (SD path)

    // ----------------------------------------------------------------
    // Step 2: Flush loaded buffer to physical PSRAM (cache coherence)
    // ----------------------------------------------------------------
    extern int Cache_WriteBack_Addr(uint32_t vaddr, uint32_t size);
    Cache_WriteBack_Addr((uint32_t)psram_buf, fw_size);

    // ----------------------------------------------------------------
    // Step 2.5: Specter secure app loader — verify the payload signature
    // ----------------------------------------------------------------
    uint32_t payload_offset = 0;
    bl_section_t *main_hdr = (bl_section_t *)psram_buf;
    if (flash_payload) {
        ESP_LOGW(TAG, "Booting raw 0xE9 image from the flash 'payload' partition "
                      "(dev/bring-up path, Specter verification skipped).");
    } else if (main_hdr->magic == BL_SECT_MAGIC) {
        ESP_LOGI(TAG, "Specter bootloader section detected");

        if (!blsect_validate_header(main_hdr)) {
            ESP_LOGE(TAG, "Invalid Specter section header. Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        char platform_str[BL_ATTR_STR_MAX] = {0};
        if (!blsect_get_attr_str(main_hdr, bl_attr_platform, platform_str,
                                 sizeof(platform_str)) ||
            strcmp(platform_str, "seedsigner_esp32s3") != 0) {
            ESP_LOGE(TAG, "Invalid platform attribute: '%s' (expected seedsigner_esp32s3). Halting.",
                     platform_str);
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "Firmware version: %lu", (unsigned long)main_hdr->pl_ver);
        if (main_hdr->pl_ver < 1) {
            ESP_LOGE(TAG, "Firmware downgrade detected! Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        // Hash the whole main section (header + payload) from PSRAM.
        // blsys_flash_read maps bl_addr_t to a direct pointer.
        bl_hash_t hash_obj;
        if (!blsect_hash_over_flash(main_hdr,
                                    (bl_addr_t)(psram_buf + sizeof(bl_section_t)),
                                    &hash_obj, 0)) {
            ESP_LOGE(TAG, "Hash calculation failed. Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        // Signature section immediately follows the main payload.
        bl_section_t *sig_hdr = (bl_section_t *)(psram_buf + sizeof(bl_section_t) +
                                                 main_hdr->pl_size);
        if (sig_hdr->magic == BL_SECT_MAGIC && blsect_is_signature(sig_hdr)) {
            uint8_t sig_msg[BL_SIG_MSG_MAX];
            size_t sig_msg_size = sizeof(sig_msg);
            if (blsect_make_signature_message(sig_msg, &sig_msg_size, &hash_obj, 1)) {
                ESP_LOGI(TAG, "Performing secp256k1 multisig verification...");
                bl_set_progress_callback(crypto_progress_cb, NULL);

                int32_t sig_res = blsig_verify_multisig(
                    "secp256k1-sha256",
                    (uint8_t *)sig_hdr + sizeof(bl_section_t), sig_hdr->pl_size,
                    pubkeys_boot, sig_msg, sig_msg_size, 0);

                if (blsig_is_error(sig_res) || sig_res < SIG_THRESHOLD) {
                    if (blsig_is_error(sig_res)) {
                        ESP_LOGE(TAG, "Signature verification failed: %s", blsig_error_text(sig_res));
                    } else {
                        ESP_LOGE(TAG, "Signature verification failed: %d valid signature(s), need %d", sig_res, SIG_THRESHOLD);
                    }
                    ESP_LOGE(TAG, "HALTING execution.");
                    memset(psram_buf, 0, fw_size);
                    while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
                }
                ESP_LOGI(TAG, "Signature verification PASSED!");
            } else {
                ESP_LOGE(TAG, "Failed to build signature message. Halting.");
                memset(psram_buf, 0, fw_size);
                while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        } else {
            ESP_LOGE(TAG, "Signature section missing or invalid. Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        payload_offset = sizeof(bl_section_t);
    } else {
        ESP_LOGE(TAG, "No Specter section header (magic 0x%08lX) — raw images are not "
                      "accepted from the SD card. Halting.",
                 (unsigned long)main_hdr->magic);
        memset(psram_buf, 0, fw_size);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // ----------------------------------------------------------------
    // Phase 13: Anti-phishing proof
    // TEMP (JMP bring-up): verify_anti_phishing_proof() SHA-256s the whole
    // random_fill partition (~2.2s per boot) — commented out to speed up the
    // flash-test cycle. Restore before merging.
    // ----------------------------------------------------------------
#if 1 // TEMP: disabled for JMP bring-up speed
    provision_flash_fill();       // no-op if already provisioned
    char words[4][12];
    esp_err_t ap_err = verify_anti_phishing_proof(words);
    if (ap_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "FLASH TAMPERED — halting boot");
        memset(psram_buf, 0, fw_size);
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
#endif

    // ----------------------------------------------------------------
    // Step 3: Validate raw ESP32 image header
    // ----------------------------------------------------------------
    __attribute__((aligned(4))) esp_image_header_t hdr;
    hdr = *(esp_image_header_t *)(psram_buf + payload_offset);

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

    // ----------------------------------------------------------------
    // Step 4: First pass — measure PSRAM MMU footprint (shared linear space)
    // ----------------------------------------------------------------
    uint32_t max_offset = 0;
    uint32_t offset     = sizeof(esp_image_header_t);

    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        memcpy(&seg, psram_buf + payload_offset + offset, sizeof(seg));
        offset += sizeof(seg);
        if (seg.data_len > MAX_FIRMWARE_SIZE) {
            ESP_LOGE(TAG, "Segment %d bad length %lu. Halting.", i, (unsigned long)seg.data_len);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        if (seg.load_addr >= 0x42000000 && seg.load_addr < 0x44000000) {
            uint32_t end = (seg.load_addr & SOC_MMU_LINEAR_ADDR_MASK) + seg.data_len;
            if (end > max_offset) max_offset = end;
        }
        if (seg.load_addr >= 0x3C000000 && seg.load_addr < 0x3E000000) {
            uint32_t end = (seg.load_addr & SOC_MMU_LINEAR_ADDR_MASK) + seg.data_len;
            if (end > max_offset) max_offset = end;
        }
        offset += seg.data_len;
    }
    max_offset = (max_offset + 0xFFFF) & ~0xFFFF;
    ESP_LOGI(TAG, "PSRAM MMU footprint: %lu bytes", (unsigned long)max_offset);

    // ----------------------------------------------------------------
    // Step 5: Allocate fake-flash staging buffer for mapped segments
    // ----------------------------------------------------------------
    uint8_t  *fake_flash  = NULL;
    uint32_t  flash_paddr = 0;

    if (max_offset > 0) {
        fake_flash = heap_caps_aligned_alloc(65536, max_offset, MALLOC_CAP_SPIRAM);
        if (!fake_flash) {
            ESP_LOGE(TAG, "fake_flash alloc failed. Halting.");
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        memset(fake_flash, 0, max_offset);

        mmu_target_t target;
        if (esp_mmu_vaddr_to_paddr(fake_flash, &flash_paddr, &target) != ESP_OK) {
            ESP_LOGE(TAG, "paddr lookup for fake_flash failed. Halting.");
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "fake_flash: vaddr=%p paddr=0x%08lX", fake_flash, (unsigned long)flash_paddr);
    }

    // ----------------------------------------------------------------
    // Step 6: Second pass — place segments and build MMU map
    // ----------------------------------------------------------------
    offset = sizeof(esp_image_header_t);
    mmu_mapping_t mmu_mappings[16];
    int      mapping_count       = 0;

    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        memcpy(&seg, psram_buf + payload_offset + offset, sizeof(seg));
        offset += sizeof(seg);

        ESP_LOGI(TAG, "Seg %d: addr=0x%08lX len=%lu",
                 i, (unsigned long)seg.load_addr, (unsigned long)seg.data_len);

        bool is_irom = (seg.load_addr >= 0x42000000 && seg.load_addr < 0x44000000);
        bool is_drom = (seg.load_addr >= 0x3C000000 && seg.load_addr < 0x3E000000);

        if (is_irom || is_drom) {
            // Mapped segment → stage into fake_flash at its linear offset
            uint32_t linear     = seg.load_addr & SOC_MMU_LINEAR_ADDR_MASK;
            uint32_t va_start   = seg.load_addr & ~0xFFFF;
            uint32_t va_end     = (seg.load_addr + seg.data_len + 0xFFFF - 1) & ~0xFFFF;
            if (seg.data_len == 0) va_end = va_start;

            mmu_mappings[mapping_count].vaddr = va_start;
            mmu_mappings[mapping_count].len   = va_end - va_start;
            mmu_mappings[mapping_count].paddr = flash_paddr + (va_start & SOC_MMU_LINEAR_ADDR_MASK);

            uint32_t write_off = linear;
            memcpy(fake_flash + write_off, psram_buf + payload_offset + offset, seg.data_len);
            mapping_count++;
            ESP_LOGI(TAG, "  -> fake_flash+0x%lX", (unsigned long)write_off);
        } else if ((seg.load_addr >= 0x40370000 && seg.load_addr < 0x403E0000) ||
                   (seg.load_addr >= 0x3FC88000 && seg.load_addr < 0x3FD00000) ||
                   (seg.load_addr >= 0x50000000 && seg.load_addr < 0x50020000) ||
                   (seg.load_addr >= 0x600fe000 && seg.load_addr < 0x60100000)) {
            // Direct segment → deferred copy into internal SRAM (or RTC fast/slow
            // memory for the app's RTC reservations, which are always
            // accessible regardless of cache/MMU state).
            // The payload's IRAM must stay below the relocated JMP zone.
            uint32_t seg_end = seg.load_addr + seg.data_len;
            if (seg.load_addr < JMP_ZONE_BASE && seg_end > JMP_ZONE_BASE) {
                ESP_LOGE(TAG, "Segment %d (0x%08lX-0x%08lX) collides with the loader's JMP zone "
                              "(0x%08X-0x%08X). Halting.",
                         i, (unsigned long)seg.load_addr, (unsigned long)seg_end,
                         JMP_ZONE_BASE, JMP_ZONE_END);
                while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            if (seg.load_addr >= JMP_ZONE_BASE && seg.load_addr < JMP_ZONE_END) {
                ESP_LOGE(TAG, "Segment %d starts inside the loader's JMP zone. Halting.", i);
                while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            safe_copies[safe_copy_count].dest = (void *)seg.load_addr;
            safe_copies[safe_copy_count].src  = (void *)(psram_buf + payload_offset + offset);
            safe_copies[safe_copy_count].len  = seg.data_len;
            safe_copy_count++;
            ESP_LOGI(TAG, "  -> direct copy to 0x%08lX", (unsigned long)seg.load_addr);
        } else {
            ESP_LOGE(TAG, "Segment %d load addr 0x%08lX outside recognized S3 memory windows. Halting.",
                     i, (unsigned long)seg.load_addr);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        offset += seg.data_len;
    }

    esp_cache_msync(fake_flash, max_offset, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

    // ----------------------------------------------------------------
    // Step 7: Commit and jump
    // NOTE: psram_buf must NOT be freed — deferred copies still point into it.
    // ----------------------------------------------------------------
    safe_entry_addr = hdr.entry_addr;
    for (int i = 0; i < mapping_count; i++) safe_mappings[i] = mmu_mappings[i];
    safe_mapping_count = mapping_count;

    // Hand the fake_flash staging buffer to the JMP zone (which runs with the
    // flash windows remapped away, so it cannot use the normal heap API).
    extern uint8_t *fake_flash_ptr;
    extern uint32_t fake_flash_len;
    fake_flash_ptr = fake_flash;
    fake_flash_len = max_offset;

    // DIAGNOSTIC: is the store visible? and what is the CPU window state right
    // before the JMP-zone entry? TODO remove.
    diag_ps("before JMP");
    ESP_LOGI(TAG, "[DBG] before JMP: entry=0x%08lX copies=%d maps=%d",
             (unsigned long)safe_entry_addr, safe_copy_count, safe_mapping_count);

    // DIAGNOSTIC: can the JMP zone execute code? TODO remove.
    jmp_prepare();
    ESP_LOGI(TAG, "[DBG] IRAM0 world0 PMS constrain reg 2 = 0x%08X (memprot off)",
             (unsigned)REG_READ(SENSITIVE_CORE_X_IRAM0_PMS_CONSTRAIN_2_REG));
    {
        volatile uint32_t *q = (volatile uint32_t *)0x403A1000;
        q[0] = 0xCAFE0000;
        ESP_LOGI(TAG, "[DBG] ir0 write/read 0x403A1000 -> %08X", q[0]);
        ESP_LOGI(TAG, "[DBG] calling jmp_zone_probe @%p ...", (void *)jmp_zone_probe);
        jmp_zone_probe();
        ESP_LOGI(TAG, "[DBG] jmp_zone_probe returned OK");
    }

    ESP_LOGI(TAG, "Jumping to 0x%08lX ...", (unsigned long)safe_entry_addr);
    for (int i = 0; i < safe_copy_count; i++)
        ESP_LOGI(TAG, "  copy[%d]: %p <- %p (%lu B)",
                 i, safe_copies[i].dest, safe_copies[i].src, (unsigned long)safe_copies[i].len);

    vTaskDelay(100 / portTICK_PERIOD_MS);
    // DIAG: vTaskDelay returned and we're about to call the JMP zone. TODO remove.
    {
        uint32_t ws, wb;
        asm volatile ("rsr %0, windowstart" : "=r"(ws));
        asm volatile ("rsr %0, windowbase" : "=r"(wb));
        ESP_LOGI(TAG, "[DBG] pre-call do_mmu_mapping_and_jump (ws=0x%X wb=%lu)",
                 (unsigned)ws, (unsigned long)wb);
    }
    // DIAG: multi-instruction naked zone code. TODO remove.
    ESP_LOGI(TAG, "[DBG] calling zone_naked_test @%p", (void *)zone_naked_test);
    zone_naked_test();
    ESP_LOGI(TAG, "[DBG] zone_naked_test returned OK");
    // DIAG: l32r literal reads inside zone code. TODO remove.
    ESP_LOGI(TAG, "[DBG] calling zone_l32r_test @%p", (void *)zone_l32r_test);
    zone_l32r_test();
    ESP_LOGI(TAG, "[DBG] zone_l32r_test returned OK");
    // DIAG: `entry` inside zone code. TODO remove.
    ESP_LOGI(TAG, "[DBG] calling zone_entry_test @%p", (void *)zone_entry_test);
    zone_entry_test();
    ESP_LOGI(TAG, "[DBG] zone_entry_test returned OK");
    // DIAG: does an entry-bearing call into the zone work? TODO remove.
    static const char zet[] = "ZET entry-in-zone OK\r\n";
    // DIAG: l8ui (byte read) from DROM inside the zone. TODO remove.
    ESP_LOGI(TAG, "[DBG] calling zone_byte_read_test @%p", (void *)zone_byte_read_test);
    zone_byte_read_test(zet);
    ESP_LOGI(TAG, "[DBG] zone_byte_read_test returned OK");
    // DIAG: bootloader_uart0_print's loop core, marked. TODO remove.
    ESP_LOGI(TAG, "[DBG] calling zone_loop_core_test @%p", (void *)zone_loop_core_test);
    zone_loop_core_test();
    ESP_LOGI(TAG, "[DBG] zone_loop_core_test returned OK");
    // DIAG: bootloader_uart0_print with an EMPTY string — prologue only, no
    // per-char loop. TODO remove.
    ESP_LOGI(TAG, "[DBG] calling bootloader_uart0_print(\"\")");
    bootloader_uart0_print("");
    ESP_LOGI(TAG, "[DBG] bootloader_uart0_print(\"\") returned");
    // DIAG: call bootloader_uart0_print from INSIDE the zone (call8). TODO remove.
    ESP_LOGI(TAG, "[DBG] calling zone_wrap_print_test @%p", (void *)zone_wrap_print_test);
    zone_wrap_print_test(zet);
    ESP_LOGI(TAG, "[DBG] zone_wrap_print_test returned OK: a2=0x%08lX a10=0x%08lX",
             (unsigned long)diag_a2_val, (unsigned long)diag_a10_val);
    // DIAG: direct call from app_main (callx8). TODO remove.
    ESP_LOGI(TAG, "[DBG] calling bootloader_uart0_print(zet)");
    bootloader_uart0_print(zet);
    ESP_LOGI(TAG, "[DBG] bootloader_uart0_print(zet) returned");
    // DIAG: RTC WDT state right before the JMP-zone entry (post-reset of ~7ms
    // after the payload handoff, rst:0x10). TODO remove.
    {
        volatile uint32_t *w = (volatile uint32_t *)0x60008000;
        ESP_LOGI(TAG, "[DBG] RTC_WDT: CFG0=%08lX CFG1=%08lX CFG4=%08lX RST_ST=%08lX",
                 (unsigned long)w[0x8C / 4], (unsigned long)w[0x90 / 4],
                 (unsigned long)w[0xA8 / 4], (unsigned long)w[0xAC / 4]);
    }
    do_mmu_mapping_and_jump();
}

// JMP-zone pointers handed from app_main (the JMP zone cannot call malloc once
// the MMU is remapped). Uninitialized — the `.jmp_zone.bss` section is NOLOAD,
// and app_main always sets both before jumping.
JMP_ZONE_BSS uint8_t *fake_flash_ptr;
JMP_ZONE_BSS uint32_t fake_flash_len;
