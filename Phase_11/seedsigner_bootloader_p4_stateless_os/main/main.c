// --- Standard library ---
#include <string.h>

// --- FreeRTOS ---
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// --- ESP-IDF: logging, heap, MMU, partition ---
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "esp_rom_sys.h"
#include "esp_rom_uart.h"

// --- ESP-IDF: CPU / WDT / timer / cache / MMU registers ---
#include "esp_cpu.h"
#include "hal/wdt_hal.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_periph.h"
#include "soc/systimer_struct.h"
#include "soc/rtc.h"
#include "riscv/rv_utils.h"
#include "hal/cache_hal.h"
#include "soc/spi_mem_c_reg.h"
#include "soc/spi_mem_s_reg.h"

// ==================== ESP32-P4 Memory Map ====================
// The ESP32-P4 is a RISC-V dual-core SoC with a unified cache
// address space for both instructions and data (I/D share vaddr).
//
// Internal SRAM (HP L2MEM):  0x4FF00000 - 0x4FFBFFFF (768 KB)
// Flash cache (IROM/DROM):   0x40000000 - 0x44000000 (I/D shared)
// PSRAM cache:               0x48000000 - 0x4C000000 (via MMU)
//
// Key difference from ESP32-S3:
//   - S3: IROM 0x42000000-0x44000000, DROM 0x3C000000-0x3E000000
//   - P4: Both IROM and DROM share 0x40000000-0x44000000
//   - S3: IRAM 0x40370000-0x403D8000, DRAM 0x3FC88000-0x3FCE9000
//   - P4: HP L2MEM 0x4FF00000-0x4FFBFFFF (unified I/D RAM)
//   - S3: Xtensa architecture, P4: RISC-V architecture
// =============================================================

static const char *TAG = "SEEDSIGNER_LOADER";
#define MAX_FIRMWARE_SIZE (8 * 1024 * 1024)

// ---------------------------------------------------------------------------
// ESP32 image structures
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  magic;
    uint8_t  segment_count;
    uint8_t  spi_mode;
    uint8_t  spi_speed : 4;
    uint8_t  spi_size  : 4;
    uint32_t entry_addr;
    uint8_t  wp_pin;
    uint8_t  spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t  min_chip_rev;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint8_t  reserved[4];
    uint8_t  hash_appended;
} __attribute__((packed)) esp_image_header_t;

typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} esp_image_segment_header_t;

// ---------------------------------------------------------------------------
// State saved in RTC RAM — survives a software reset, not a power cycle
// ---------------------------------------------------------------------------
typedef struct { void *dest; void *src; uint32_t len; } pending_copy_t;
typedef struct { uint32_t vaddr; uint32_t paddr; uint32_t len; } mmu_mapping_t;

RTC_DATA_ATTR static pending_copy_t safe_copies[20];
RTC_DATA_ATTR static int            safe_copy_count    = 0;
RTC_DATA_ATTR static uint32_t       safe_entry_addr    = 0;
RTC_DATA_ATTR static mmu_mapping_t  safe_mappings[20];
RTC_DATA_ATTR static int            safe_mapping_count = 0;

// 64 KB scratch buffer used to thrash the L1 D-cache before jump
static uint32_t evict_buf[65536 / 4];

// ---------------------------------------------------------------------------
// ROM-safe helpers (stack strings only; callable with cache disabled)
// ---------------------------------------------------------------------------
static void RTC_IRAM_ATTR bootloader_uart0_print(const char *str)
{
    if (!str) return;

    volatile uint32_t *uart0_fifo   = (volatile uint32_t *)0x500CA000;
    volatile uint32_t *uart0_status = (volatile uint32_t *)0x500CA01C;

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

static void RTC_IRAM_ATTR dbg_print_hex(uint32_t val)
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
static void RTC_IRAM_ATTR __attribute__((noreturn)) do_mmu_mapping_and_jump(void)
{
    char msg5[]   = "JMP[7] JUMP!\r\n";
    char m_enter[] = "JMP[1] entered\r\n";
    bootloader_uart0_print(m_enter);

    portDISABLE_INTERRUPTS();

    // Clear FreeRTOS hardware watchpoints / PMP
    esp_cpu_clear_watchpoint(0);
    esp_cpu_clear_watchpoint(1);

    // Disable all watchdogs (SWD, LP_WDT, MWDT0, MWDT1, RWDT)
    REG_WRITE(0x50116020, 0x50D83AA1);
    REG_WRITE(0x5011601C, (1U << 31) | (1U << 30) | (1U << 19) | (1U << 18));
    REG_WRITE(0x50116020, 0);

    REG_WRITE(0x50116018, 0x50D83AA1);
    REG_WRITE(0x50116014, (1U << 31));
    REG_CLR_BIT(0x50116000, (1U << 31) | (0xFFFU << 19) | (1U << 12));
    REG_WRITE(0x5011602C, 0);
    REG_WRITE(0x50116030, 0xFFFFFFFF);
    REG_WRITE(0x50116018, 0);

    TIMERG0.wdtwprotect.val    = 0x50D83AA1;
    TIMERG0.wdtfeed.val        = 1;
    TIMERG0.wdtconfig0.val     = 0;
    TIMERG0.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG0.wdtwprotect.val    = 0;

    TIMERG1.wdtwprotect.val    = 0x50D83AA1;
    TIMERG1.wdtfeed.val        = 1;
    TIMERG1.wdtconfig0.val     = 0;
    TIMERG1.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG1.wdtwprotect.val    = 0;

    wdt_hal_context_t mwdt0_ctx = {.inst = WDT_MWDT0, .mwdt_dev = &TIMERG0};
    wdt_hal_write_protect_disable(&mwdt0_ctx);
    wdt_hal_disable(&mwdt0_ctx);
    wdt_hal_write_protect_enable(&mwdt0_ctx);

    wdt_hal_context_t mwdt1_ctx = {.inst = WDT_MWDT1, .mwdt_dev = &TIMERG1};
    wdt_hal_write_protect_disable(&mwdt1_ctx);
    wdt_hal_disable(&mwdt1_ctx);
    wdt_hal_write_protect_enable(&mwdt1_ctx);

    wdt_hal_context_t rwdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rwdt_ctx);
    wdt_hal_disable(&rwdt_ctx);
    wdt_hal_write_protect_enable(&rwdt_ctx);

    // Silence SYSTIMER and timer-group interrupts
    SYSTIMER.int_ena.val  = 0;
    SYSTIMER.int_clr.val  = 0x7;
    SYSTIMER.conf.val    &= ~((1U << 22) | (1U << 23) | (1U << 24));
    TIMERG0.int_ena_timers.val = 0;
    TIMERG0.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG1.int_ena_timers.val = 0;
    TIMERG1.int_clr_timers.val = 0xFFFFFFFF;

    char m_wdt[] = "JMP[2] WDT and SysTick disabled\r\n";
    bootloader_uart0_print(m_wdt);

    asm volatile ("csrw mie, zero\n");
    rv_utils_intr_global_disable();

    char m_intr[] = "JMP[3] interrupts off, starting cache eviction\r\n";
    bootloader_uart0_print(m_intr);

    // Thrash full 64 KB L1 D-cache to force eviction before MMU remap
    volatile uint32_t *evict_ptr = evict_buf;
    for (int i = 0; i < (65536 / 4); i++) evict_ptr[i] = i;
    char m_wb[] = "JMP[4] D-cache evicted\r\n";
    bootloader_uart0_print(m_wb);

    // Copy direct segments into internal SRAM (source is in PSRAM — cache still up)
    for (int i = 0; i < safe_copy_count; i++) {
        uint8_t *d = (uint8_t *)safe_copies[i].dest;
        uint8_t *s = (uint8_t *)safe_copies[i].src;
        for (uint32_t j = 0; j < safe_copies[i].len; j++) d[j] = s[j];
    }

    char m_copy[] = "JMP[5] copies done, entry bytes: ";
    bootloader_uart0_print(m_copy);
    dbg_print_hex(*(volatile uint32_t *)safe_entry_addr);
    char m_nl[] = "\r\n";
    bootloader_uart0_print(m_nl);

    // Program MMU entries for PSRAM-mapped segments
    for (int i = 0; i < safe_mapping_count; i++) {
        uint32_t page_size = 65536;
        uint32_t page_num  = (safe_mappings[i].len + page_size - 1) / page_size;
        uint32_t vaddr     = safe_mappings[i].vaddr;
        uint32_t mmu_val   = safe_mappings[i].paddr >> 16;

        char m_map[] = "JMP[6] MMU map: vaddr=";
        bootloader_uart0_print(m_map);
        dbg_print_hex(vaddr);
        char m_pa[] = " paddr=";
        bootloader_uart0_print(m_pa);
        dbg_print_hex(safe_mappings[i].paddr);
        char m_pg[] = " pages=";
        bootloader_uart0_print(m_pg);
        dbg_print_hex(page_num);
        bootloader_uart0_print(m_nl);

        while (page_num--) {
            uint32_t entry_id    = (vaddr & 0x03FFFFFF) >> 16;
            uint32_t index_reg   = SPI_MEM_C_MMU_ITEM_INDEX_REG;
            uint32_t content_reg = SPI_MEM_C_MMU_ITEM_CONTENT_REG;
            uint32_t final_val   = mmu_val | (1 << 10);

            if (vaddr >= 0x48000000) {
                index_reg   = SPI_MEM_S_MMU_ITEM_INDEX_REG;
                content_reg = SPI_MEM_S_MMU_ITEM_CONTENT_REG;
                // PSRAM: valid | access | no sensitive (plaintext)
                final_val   = mmu_val | (1 << 11) | (1 << 10);
            }

            char m_ent[] = "  entry=";
            bootloader_uart0_print(m_ent);
            dbg_print_hex(entry_id);
            char m_val[] = " val=";
            bootloader_uart0_print(m_val);
            dbg_print_hex(final_val);
            REG_WRITE(index_reg, entry_id);
            REG_WRITE(content_reg, final_val);
            char m_ok[] = " OK\r\n";
            bootloader_uart0_print(m_ok);

            vaddr   += page_size;
            mmu_val++;
        }
    }

    // Invalidate caches for remapped region
    extern int Cache_Invalidate_Addr(uint32_t map, uint32_t addr, uint32_t size);
    if (safe_mapping_count == 0) {
        Cache_Invalidate_Addr(0x03, 0x48000000, 0x40000);
        Cache_Invalidate_Addr(0x10, 0x48000000, 0x40000);
        Cache_Invalidate_Addr(0x20, 0x48000000, 0x40000);
    } else {
        for (int i = 0; i < safe_mapping_count; i++) {
            Cache_Invalidate_Addr(0x03, safe_mappings[i].vaddr, safe_mappings[i].len);
            Cache_Invalidate_Addr(0x10, safe_mappings[i].vaddr, safe_mappings[i].len);
            Cache_Invalidate_Addr(0x20, safe_mappings[i].vaddr, safe_mappings[i].len);
        }
    }
    asm volatile ("fence.i\n");

    bootloader_uart0_print(msg5);

    // Drain UART TX FIFO before jumping
    esp_rom_output_tx_wait_idle(0);
    volatile uint32_t *uart_status = (volatile uint32_t *)0x500CA01C;
    volatile uint32_t drain_timeout = 1000000;
    while (((*uart_status >> 16) & 0xFF) > 0 && --drain_timeout > 0) {}

    // Final interrupt disable + CSR/PMP teardown
    asm volatile ("csrw mie, zero\n");
    rv_utils_intr_global_disable();
    asm volatile (
        "csrw mtvec, zero\n"
        "csrw 0x3a0, zero\n" "csrw 0x3a1, zero\n"
        "csrw 0x3a2, zero\n" "csrw 0x3a3, zero\n"
        "csrw 0x3b0, zero\n" "csrw 0x3b1, zero\n"
        "csrw 0x3b2, zero\n" "csrw 0x3b3, zero\n"
        "csrw 0x3b4, zero\n" "csrw 0x3b5, zero\n"
        "csrw 0x3b6, zero\n" "csrw 0x3b7, zero\n"
        "csrw 0x3b8, zero\n" "csrw 0x3b9, zero\n"
        "csrw 0x3ba, zero\n" "csrw 0x3bb, zero\n"
        "csrw 0x3bc, zero\n" "csrw 0x3bd, zero\n"
        "csrw 0x3be, zero\n" "csrw 0x3bf, zero\n"
    );

    typedef void (*entry_t)(void) __attribute__((noreturn));
    ((entry_t)safe_entry_addr)();
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "SeedSigner Loader — ESP32-P4 PSRAM payload");

    uint8_t *psram_buf = NULL;
    size_t   fw_size   = 0;

    // ----------------------------------------------------------------
    // Step 1: Load firmware from flash 'payload' partition @ 0x140000
    // ----------------------------------------------------------------
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0xFF, "payload");
    if (part == NULL) {
        ESP_LOGE(TAG, "'payload' partition not found. Halting.");
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    uint8_t magic = 0;
    if (esp_partition_read(part, 0, &magic, 1) != ESP_OK || magic != 0xE9) {
        ESP_LOGE(TAG, "'payload' partition empty or invalid magic (0x%02X). Halting.", magic);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    fw_size   = (part->size < MAX_FIRMWARE_SIZE) ? part->size : MAX_FIRMWARE_SIZE;
    psram_buf = heap_caps_aligned_alloc(65536, fw_size, MALLOC_CAP_SPIRAM);
    if (!psram_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%lu bytes). Halting.", (unsigned long)fw_size);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    if (esp_partition_read(part, 0, psram_buf, fw_size) != ESP_OK) {
        ESP_LOGE(TAG, "Partition read failed. Halting.");
        free(psram_buf);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "[FLASH PARTITION] Loaded %lu bytes from 'payload' @ 0x%08lX",
             (unsigned long)fw_size, (unsigned long)part->address);

    // ----------------------------------------------------------------
    // Step 2: Flush loaded buffer to physical PSRAM (cache coherence)
    // ----------------------------------------------------------------
    extern int Cache_WriteBack_Addr(uint32_t map, uint32_t vaddr, uint32_t size);
    Cache_WriteBack_Addr(0x10, (uint32_t)psram_buf, fw_size);
    Cache_WriteBack_Addr(0x20, (uint32_t)psram_buf, fw_size);

    // ----------------------------------------------------------------
    // Step 3: Validate raw ESP32 image header
    // ----------------------------------------------------------------
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

    // ----------------------------------------------------------------
    // Step 4: First pass — measure PSRAM MMU footprint
    // ----------------------------------------------------------------
    uint32_t max_offset = 0;
    uint32_t offset     = sizeof(esp_image_header_t);

    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        memcpy(&seg, psram_buf + offset, sizeof(seg));
        offset += sizeof(seg);
        if (seg.data_len > MAX_FIRMWARE_SIZE) {
            ESP_LOGE(TAG, "Segment %d bad length %lu. Halting.", i, (unsigned long)seg.data_len);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        if (seg.load_addr >= 0x48000000 && seg.load_addr < 0x4C000000) {
            uint32_t end = (seg.load_addr - 0x48000000) + seg.data_len;
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
    uint32_t last_page_start     = 0xFFFFFFFF;

    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        memcpy(&seg, psram_buf + offset, sizeof(seg));
        offset += sizeof(seg);

        ESP_LOGI(TAG, "Seg %d: addr=0x%08lX len=%lu",
                 i, (unsigned long)seg.load_addr, (unsigned long)seg.data_len);

        if (seg.load_addr >= 0x48000000 && seg.load_addr < 0x4C000000) {
            // Mapped segment → stage into fake_flash
            uint32_t va_start = seg.load_addr & ~0xFFFF;
            uint32_t va_end   = (seg.load_addr + seg.data_len + 0xFFFF - 1) & ~0xFFFF;
            if (seg.data_len == 0) va_end = va_start;

            mmu_mappings[mapping_count].vaddr = va_start;
            mmu_mappings[mapping_count].len   = va_end - va_start;
            mmu_mappings[mapping_count].paddr = flash_paddr + (va_start - 0x48000000);

            uint32_t write_off = seg.load_addr - 0x48000000;
            memcpy(fake_flash + write_off, psram_buf + offset, seg.data_len);

            uint32_t page_start = write_off & ~0xFFFF;
            if (page_start != last_page_start) {
                memcpy(fake_flash + page_start, psram_buf, 32); // image header at page start
                last_page_start = page_start;
            }
            mapping_count++;
            ESP_LOGI(TAG, "  -> fake_flash+0x%lX", (unsigned long)write_off);
        } else {
            // Direct segment → deferred copy into internal SRAM
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

    // ----------------------------------------------------------------
    // Step 7: Commit and jump
    // NOTE: psram_buf must NOT be freed — deferred copies still point into it.
    // ----------------------------------------------------------------
    safe_entry_addr = hdr.entry_addr;
    for (int i = 0; i < mapping_count; i++) safe_mappings[i] = mmu_mappings[i];
    safe_mapping_count = mapping_count;

    ESP_LOGI(TAG, "Jumping to 0x%08lX ...", (unsigned long)safe_entry_addr);
    for (int i = 0; i < safe_copy_count; i++)
        ESP_LOGI(TAG, "  copy[%d]: %p <- %p (%lu B)",
                 i, safe_copies[i].dest, safe_copies[i].src, (unsigned long)safe_copies[i].len);

    vTaskDelay(100 / portTICK_PERIOD_MS);
    do_mmu_mapping_and_jump();
}
