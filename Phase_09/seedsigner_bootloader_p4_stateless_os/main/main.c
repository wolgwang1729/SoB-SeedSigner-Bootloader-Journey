#include "esp_flash.h"
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_partition.h"

#include "bl_section.h"

#include "bl_signature.h"
#include "bl_syscalls.h"

#include "esp_mmu_map.h"
#include "hal/mmu_hal.h"
#include "hal/cache_ll.h"
#include "esp_rom_spiflash.h"
#include "esp_rom_sys.h"
#include "esp_rom_uart.h"

// ==================== ESP32-P4 Memory Map ====================
// The ESP32-P4 is a RISC-V dual-core SoC with a unified cache
// address space for both instructions and data (I/D share vaddr).
//
// Internal SRAM (HP L2MEM):  0x4FF00000 - 0x4FFBFFFF (768 KB)
// Flash cache (IROM/DROM):   0x40000000 - 0x44000000 (I/D shared)
// PSRAM cache:               0x48000000 - 0x4C000000 (via offset)
//
// Key difference from ESP32-S3:
//   - S3: IROM 0x42000000-0x44000000, DROM 0x3C000000-0x3E000000
//   - P4: Both IROM and DROM share 0x40000000-0x44000000
//   - S3: IRAM 0x40370000-0x403D8000, DRAM 0x3FC88000-0x3FCE9000
//   - P4: HP L2MEM 0x4FF00000-0x4FFBFFFF (unified I/D RAM)
//   - S3: Xtensa architecture, P4: RISC-V architecture
// =============================================================

static const char *TAG = "SEEDSIGNER_LOADER";
#define MOUNT_POINT "/sdcard"

// Dummy keys for verification (so it passes test keys)
const bl_pubkey_t vendor_keys[] = {
    { .bytes = { 0x04, 0x08, 0xf4, 0xf3, 0x7e, 0x2d, 0x8f, 0x74, 0xe1, 0x8c, 0x1b, 0x8f, 0xde, 0x23, 0x74, 0xd5, 0xf2, 0x84, 0x02, 0xfb, 0x8a, 0xb7, 0xfd, 0x1c, 0xc5, 0xb7, 0x86, 0xaa, 0x40, 0x85, 0x1a, 0x70, 0xcb, 0xc2, 0xec, 0xa8, 0x7b, 0x8b, 0xd2, 0xc0, 0xbe, 0x52, 0x69, 0x8e, 0x9d, 0x5e, 0xe1, 0x98, 0x40, 0xc4, 0xd4, 0x0c, 0xa6, 0x96, 0xe1, 0x61, 0x59, 0x13, 0x47, 0x69, 0xfa, 0x1a, 0xe8, 0x5b, 0x2e } },
    BL_PUBKEY_END_OF_LIST
};
const bl_pubkey_t* pubkeys_boot[] = { vendor_keys, NULL };

typedef struct {
    uint8_t magic;
    uint8_t segment_count;
    uint8_t spi_mode;
    uint8_t spi_speed: 4;
    uint8_t spi_size: 4;
    uint32_t entry_addr;
    uint8_t wp_pin;
    uint8_t spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t min_chip_rev;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint8_t reserved[4];
    uint8_t hash_appended;
} __attribute__((packed)) custom_esp_image_header_t;

typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} custom_esp_image_segment_header_t;

// ESP32-P4 memory map:
// Flash-mapped cache: 0x40000000 - 0x44000000 (I/D unified)
// PSRAM-mapped cache: 0x48000000 - 0x4C000000
// On P4, IROM and DROM share the same virtual address range.
#ifndef SOC_IROM_LOW
#define SOC_IROM_LOW    0x40000000
#endif
#ifndef SOC_IROM_HIGH
#define SOC_IROM_HIGH   0x44000000
#endif
#ifndef SOC_DROM_LOW
#define SOC_DROM_LOW    0x40000000
#endif
#ifndef SOC_DROM_HIGH
#define SOC_DROM_HIGH   0x44000000
#endif

// ESP32-P4 HP L2MEM (internal SRAM) range
#define SOC_HP_L2MEM_LOW   0x4FF00000
#define SOC_HP_L2MEM_HIGH  0x4FFC0000

#define MAX_FIRMWARE_SIZE (4 * 1024 * 1024)

typedef struct {
    void *dest;
    void *src;
    uint32_t len;
} pending_copy_t;

typedef struct {
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t len;
} mmu_mapping_t;

RTC_DATA_ATTR static pending_copy_t safe_copies[20];
RTC_DATA_ATTR static int safe_copy_count = 0;
RTC_DATA_ATTR static uint32_t safe_entry_addr = 0;
RTC_DATA_ATTR static mmu_mapping_t safe_mappings[20];
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
#include "soc/uart_reg.h"

RTC_DATA_ATTR static int safe_mapping_count = 0;
static uint32_t evict_buf[65536 / 4];

static void RTC_IRAM_ATTR bootloader_dual_print(const char *str) {
    if (!str) return;

    volatile uint32_t *uart0_fifo       = (volatile uint32_t *)0x500CA000;
    volatile uint32_t *uart0_status     = (volatile uint32_t *)0x500CA01C;
    volatile uint32_t *usb_jtag_fifo_cd = (volatile uint32_t *)0x500CD000;
    volatile uint32_t *usb_jtag_status  = (volatile uint32_t *)0x500CD004;

    while (*str) {
        char ch = *str++;

        // 1. Hardware UART0 output
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

        // 2. USB-JTAG-Serial output (non-blocking status check with WR_DONE)
        uint32_t usb_st = *usb_jtag_status;
        if (usb_st == 0xFFFFFFFF) {
            *usb_jtag_fifo_cd = (uint32_t)ch;
            *usb_jtag_status = 1U; // USB_SERIAL_JTAG_WR_DONE
        } else {
            volatile uint32_t usb_timeout = 1000;
            while (!(*usb_jtag_status & (1U << 1)) && --usb_timeout > 0) {}
            if (*usb_jtag_status & (1U << 1)) {
                *usb_jtag_fifo_cd = (uint32_t)ch;
                *usb_jtag_status = 1U; // USB_SERIAL_JTAG_WR_DONE
            }
        }
    }
}

// ROM-safe hex print helper (stack strings only, works with cache disabled)
static void RTC_IRAM_ATTR dbg_print_hex(uint32_t val) {
    char hex[] = "0x00000000";
    char digits[] = "0123456789ABCDEF";
    for (int i = 9; i >= 2; i--) {
        hex[i] = digits[val & 0xF];
        val >>= 4;
    }
    bootloader_dual_print(hex);
    esp_rom_printf(hex);
}

static void RTC_IRAM_ATTR __attribute__((noreturn)) do_mmu_mapping_and_jump(void)
{
    char msg5[] = "JMP[13] JUMP!\r\n";
    
    char m_enter[] = "JMP[1] entered\r\n";
    bootloader_dual_print(m_enter);
    esp_rom_printf(m_enter);

    portDISABLE_INTERRUPTS();
    
    // Clear hardware watchpoints/PMP which were used by FreeRTOS for stack protection.
    esp_cpu_clear_watchpoint(0);
    esp_cpu_clear_watchpoint(1);

    // Disable all Watchdog Timers (LP_WDT, SWD, MWDT0, MWDT1, RWDT).
    // 1. SWD (Super Watchdog) Unlock, Feed, Clear Reset Flag, Auto-Feed Enable, and Disable
    REG_WRITE(0x50116020, 0x50D83AA1);
    REG_WRITE(0x5011601C, (1U << 31) | (1U << 30) | (1U << 19) | (1U << 18));
    REG_WRITE(0x50116020, 0);

    // 2. LP_WDT (Low Power Watchdog / RTC WDT) Unlock, Feed, Clear Flashboot & Config, Disable Interrupts
    REG_WRITE(0x50116018, 0x50D83AA1);
    REG_WRITE(0x50116014, (1U << 31));
    REG_CLR_BIT(0x50116000, (1U << 31) | (0xFFFU << 19) | (1U << 12));
    REG_WRITE(0x5011602C, 0);
    REG_WRITE(0x50116030, 0xFFFFFFFF);
    REG_WRITE(0x50116018, 0);

    // 3. MWDT0 and MWDT1 Timer Group Watchdogs Unlock, Feed, Clear Config & Interrupts
    TIMERG0.wdtwprotect.val = 0x50D83AA1;
    TIMERG0.wdtfeed.val = 1;
    TIMERG0.wdtconfig0.val = 0;
    TIMERG0.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG0.wdtwprotect.val = 0;

    TIMERG1.wdtwprotect.val = 0x50D83AA1;
    TIMERG1.wdtfeed.val = 1;
    TIMERG1.wdtconfig0.val = 0;
    TIMERG1.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG1.wdtwprotect.val = 0;

    wdt_hal_context_t mwdt_ctx = {.inst = WDT_MWDT0, .mwdt_dev = &TIMERG0};
    wdt_hal_write_protect_disable(&mwdt_ctx);
    wdt_hal_disable(&mwdt_ctx);
    wdt_hal_write_protect_enable(&mwdt_ctx);

    wdt_hal_context_t mwdt1_ctx = {.inst = WDT_MWDT1, .mwdt_dev = &TIMERG1};
    wdt_hal_write_protect_disable(&mwdt1_ctx);
    wdt_hal_disable(&mwdt1_ctx);
    wdt_hal_write_protect_enable(&mwdt1_ctx);

    wdt_hal_context_t rtc_wdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_disable(&rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&rtc_wdt_ctx);

    // Disable and silence SysTick (SYSTIMER), MWDT0, MWDT1, and RWDT interrupts
    SYSTIMER.int_ena.val = 0;
    SYSTIMER.int_clr.val = 0x7;
    SYSTIMER.conf.val &= ~((1U << 22) | (1U << 23) | (1U << 24));

    TIMERG0.int_ena_timers.val = 0;
    TIMERG0.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG1.int_ena_timers.val = 0;
    TIMERG1.int_clr_timers.val = 0xFFFFFFFF;

    char m_wdt[] = "JMP[2] WDT and SysTick disabled\r\n";
    bootloader_dual_print(m_wdt);
    esp_rom_printf(m_wdt);

    asm volatile ("csrw mie, zero\n");
    rv_utils_intr_global_disable();

    char m_intr[] = "JMP[3] interrupts off, starting copies\r\n";
    bootloader_dual_print(m_intr);
    esp_rom_printf(m_intr);

    // Do the data copies into internal RAM BEFORE disabling the cache,
    // because the source buffers are in PSRAM.
    for (int i = 0; i < safe_copy_count; i++) {
        uint8_t *d = (uint8_t *)safe_copies[i].dest;
        uint8_t *s = (uint8_t *)safe_copies[i].src;
        for (uint32_t j = 0; j < safe_copies[i].len; j++) {
            d[j] = s[j];
        }
    }

    char m_copy[] = "JMP[4] copies done, verifying entry point bytes: ";
    bootloader_dual_print(m_copy);
    esp_rom_printf(m_copy);
    // Verify the entry point has non-zero data after copy
    uint32_t entry_word = *(volatile uint32_t *)safe_entry_addr;
    dbg_print_hex(entry_word);
    char m_nl[] = "\r\n";
    bootloader_dual_print(m_nl);
    esp_rom_printf(m_nl);
    
    // === L1 D-CACHE FLUSH VIA EVICTION THRASHING & EXPLICIT WRITEBACK ===
    // Force eviction across full 64KB L1 D-Cache (MAX_L1_DCACHE_SIZE = 64 * 1024).
    volatile uint32_t *evict_ptr = evict_buf;
    for (int i = 0; i < (65536 / 4); i++) {
        evict_ptr[i] = i; // Write to every word to guarantee cache line fills
    }
    char m_wb[] = "JMP[5] D-cache evicted via thrash\r\n";
    bootloader_dual_print(m_wb);
    esp_rom_printf(m_wb);
    
    for (int i = 0; i < safe_mapping_count; i++) {
        uint32_t page_size = 65536; // ESP32-P4 MMU page size
        uint32_t page_num = (safe_mappings[i].len + page_size - 1) / page_size;
        uint32_t vaddr = safe_mappings[i].vaddr;
        uint32_t paddr = safe_mappings[i].paddr;
        uint32_t mmu_val = paddr >> 16;
        
        char m_map[] = "JMP[8] MMU map: vaddr=";
        bootloader_dual_print(m_map);
        esp_rom_printf(m_map);
        dbg_print_hex(vaddr);
        char m_pa[] = " paddr=";
        bootloader_dual_print(m_pa);
        esp_rom_printf(m_pa);
        dbg_print_hex(paddr);
        char m_pg[] = " pages=";
        bootloader_dual_print(m_pg);
        esp_rom_printf(m_pg);
        dbg_print_hex(page_num);
        bootloader_dual_print(m_nl);
        esp_rom_printf(m_nl);
        
        while (page_num) {
            uint32_t entry_id = (vaddr & 0x03FFFFFF) >> 16;
            
            uint32_t index_reg = SPI_MEM_C_MMU_ITEM_INDEX_REG;
            uint32_t content_reg = SPI_MEM_C_MMU_ITEM_CONTENT_REG;
            uint32_t final_mmu_val = mmu_val | (1 << 10);
            
            if (vaddr >= 0x48000000) {
                index_reg = SPI_MEM_S_MMU_ITEM_INDEX_REG;
                content_reg = SPI_MEM_S_MMU_ITEM_CONTENT_REG;
                // Unencrypted PSRAM pages mapped as plaintext memory: remove (1 << 12) (SOC_MMU_PSRAM_SENSITIVE)
                final_mmu_val = mmu_val | (1 << 11) | (1 << 10);
            }
            
            char m_ent[] = "  entry=";
            bootloader_dual_print(m_ent);
            esp_rom_printf(m_ent);
            dbg_print_hex(entry_id);
            char m_val[] = " val=";
            bootloader_dual_print(m_val);
            esp_rom_printf(m_val);
            dbg_print_hex(final_mmu_val);

            char m_wr1[] = " W1";
            bootloader_dual_print(m_wr1);
            esp_rom_printf(m_wr1);
            REG_WRITE(index_reg, entry_id);
            char m_wr2[] = " W2";
            bootloader_dual_print(m_wr2);
            esp_rom_printf(m_wr2);
            REG_WRITE(content_reg, final_mmu_val);
            char m_ok[] = " OK\r\n";
            bootloader_dual_print(m_ok);
            esp_rom_printf(m_ok);
            
            vaddr += page_size;
            mmu_val++;
            page_num--;
        }
    }
    
    // === POST-MMU REMAP ===
    // NO FLASH STRING LITERALS ALLOWED HERE! MMU is remapped.
    // Invalidate L1 I-Cache (0x03), L1 D-Cache (0x10), and L2 Cache (0x20) for mapped payload memory range.
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
    bootloader_dual_print(msg5);
    esp_rom_printf(msg5);
    
    for (volatile int i = 0; i < 200000; i++) {}
    bootloader_dual_print(msg5);
    esp_rom_printf(msg5);

    // Ensure UART0 TX FIFO is completely drained before jumping to payload
    esp_rom_output_tx_wait_idle(0);
    volatile uint32_t *uart_status = (volatile uint32_t *)0x500CA01C;
    volatile uint32_t timeout = 1000000;
    while (((*uart_status >> 16) & 0xFF) > 0 && --timeout > 0) {}

    // Continuous interrupt disable prior to jumping to entry point
    asm volatile ("csrw mie, zero\n");
    rv_utils_intr_global_disable();

    // Reset mtvec CSR to zero and clear all PMP config and address registers before handoff
    asm volatile (
        "csrw mtvec, zero\n"
        "csrw 0x3a0, zero\n"
        "csrw 0x3a1, zero\n"
        "csrw 0x3a2, zero\n"
        "csrw 0x3a3, zero\n"
        "csrw 0x3b0, zero\n"
        "csrw 0x3b1, zero\n"
        "csrw 0x3b2, zero\n"
        "csrw 0x3b3, zero\n"
        "csrw 0x3b4, zero\n"
        "csrw 0x3b5, zero\n"
        "csrw 0x3b6, zero\n"
        "csrw 0x3b7, zero\n"
        "csrw 0x3b8, zero\n"
        "csrw 0x3b9, zero\n"
        "csrw 0x3ba, zero\n"
        "csrw 0x3bb, zero\n"
        "csrw 0x3bc, zero\n"
        "csrw 0x3bd, zero\n"
        "csrw 0x3be, zero\n"
        "csrw 0x3bf, zero\n"
    );

    typedef void (*entry_t)(void) __attribute__((noreturn));
    entry_t entry = (entry_t)safe_entry_addr;
    (*entry)();
}



// Helper for SD Card mount via SDMMC (native interface on ESP32-P4)
// The ESP32-P4 has a dedicated SDMMC peripheral with default pins:
//   CLK=GPIO43, CMD=GPIO44, D0=GPIO39, D1=GPIO40, D2=GPIO41, D3=GPIO42
sdmmc_card_t* mount_storage_sdcard(void) {
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card = NULL;
    ESP_LOGI(TAG, "Initializing SD card via SDMMC (native P4 interface)");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    
    // Power on the SD card LDO (LDO4 on ESP32-P4)
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create a new on-chip LDO power control driver");
    } else {
        host.pwr_ctrl_handle = pwr_ctrl_handle;
    }

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    // Use 1-bit mode for maximum compatibility during bring-up
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (%s).", esp_err_to_name(ret));
        return NULL;
    }
    ESP_LOGI(TAG, "SD Card mounted successfully.");
    return card;
}

// Progress callback to feed watchdog during crypto ops
void crypto_progress_cb(void* ctx, bl_cbarg_t arg, uint32_t total, uint32_t complete) {
    // In ESP-IDF v5, the main task isn't auto-subscribed to TWDT. 
    // esp_task_wdt_reset() causes "task not found" spam. Yield instead.
    vTaskDelay(1); 
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SeedSigner Secure Loader (App Mode) — ESP32-P4");

    uint8_t *psram_buf = NULL;
    size_t fw_size = 0;

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0xFF, "payload");
    bool loaded_from_flash = false;
    
    if (part != NULL) {
        ESP_LOGI(TAG, "Checking 'payload' partition in Flash (offset 0x%08lx, size %lu)...", (unsigned long)part->address, (unsigned long)part->size);
        bl_section_t tmp_hdr;
        if (esp_partition_read(part, 0, &tmp_hdr, sizeof(tmp_hdr)) == ESP_OK) {
            if (tmp_hdr.magic == BL_SECT_MAGIC && tmp_hdr.pl_size > 0 && tmp_hdr.pl_size < MAX_FIRMWARE_SIZE) {
                ESP_LOGI(TAG, "Found valid Specter Bootloader header in Flash partition! Bypassing SD Card.");
                fw_size = tmp_hdr.pl_size + sizeof(bl_section_t) + 4096; // include signature section
                if (fw_size > part->size) fw_size = part->size;
                
                psram_buf = heap_caps_aligned_alloc(65536, fw_size, MALLOC_CAP_SPIRAM);
                if (!psram_buf) {
                    ESP_LOGE(TAG, "Failed to allocate PSRAM for Flash payload.");
                } else if (esp_partition_read(part, 0, psram_buf, fw_size) == ESP_OK) {
                    loaded_from_flash = true;
                } else {
                    ESP_LOGE(TAG, "Failed to read payload from Flash partition.");
                    free(psram_buf);
                    psram_buf = NULL;
                }
            } else {
                ESP_LOGI(TAG, "No valid magic in 'payload' partition (found 0x%08lx). Falling back to SD Card...", (unsigned long)tmp_hdr.magic);
            }
        }
    }

    if (!loaded_from_flash) {
        sdmmc_card_t *card = mount_storage_sdcard();
        if (!card) return;

        const char *fw_path = MOUNT_POINT "/seed.bin";
        struct stat st;
        if (stat(fw_path, &st) != 0) {
            ESP_LOGE(TAG, "Firmware file %s not found. Halting.", fw_path);
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
            return;
        }
        
        if (st.st_size == 0 || st.st_size > MAX_FIRMWARE_SIZE) {
            ESP_LOGE(TAG, "Invalid firmware size: %ld", (long)st.st_size);
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
            return;
        }
        
        fw_size = st.st_size;
        ESP_LOGI(TAG, "Found firmware on SD Card. Size: %lu bytes", (unsigned long)fw_size);

        psram_buf = heap_caps_aligned_alloc(65536, fw_size, MALLOC_CAP_SPIRAM);
        if (!psram_buf) {
            ESP_LOGE(TAG, "Failed to allocate PSRAM for firmware buffer.");
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
            return;
        }

        FILE *f = fopen(fw_path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open firmware file.");
            free(psram_buf);
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
            return;
        }
        
        if (fread(psram_buf, 1, fw_size, f) != fw_size) {
            ESP_LOGE(TAG, "Failed to read firmware file entirely.");
            fclose(f);
            free(psram_buf);
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
            return;
        }
        fclose(f);

        ESP_LOGI(TAG, "Unmounting storage to prevent TOCTOU attacks...");
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    }

    extern int Cache_WriteBack_Addr(uint32_t map, uint32_t vaddr, uint32_t size);
    Cache_WriteBack_Addr(0x10, (uint32_t)psram_buf, fw_size);
    Cache_WriteBack_Addr(0x20, (uint32_t)psram_buf, fw_size);

    // Re-integrate signature verification
    bl_addr_t main_pl_addr = (bl_addr_t)psram_buf;
    (void)main_pl_addr; (void)fw_size;
    uint32_t payload_offset = 0;
    __attribute__((aligned(4))) custom_esp_image_header_t header_copy;

    bl_section_t *main_hdr = (bl_section_t *)psram_buf;
    if (main_hdr->magic == BL_SECT_MAGIC) {
        ESP_LOGI(TAG, "Found Specter Bootloader header.");
        
        char platform_str[33] = {0};
        if (blsect_get_attr_str(main_hdr, bl_attr_platform, platform_str, sizeof(platform_str))) {
            if (strcmp(platform_str, "seedsigner_esp32p4") != 0) {
                ESP_LOGE(TAG, "Invalid platform attribute: %s. Expected: seedsigner_esp32p4", platform_str);
                memset(psram_buf, 0, fw_size);
                while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
            }
        } else {
            ESP_LOGE(TAG, "Missing platform attribute in payload signature.");
            memset(psram_buf, 0, fw_size);
            while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
        }
        ESP_LOGI(TAG, "Firmware version: %lu", (unsigned long)main_hdr->pl_ver);
        if (main_hdr->pl_ver < 1) {
            ESP_LOGE(TAG, "Firmware downgrade detected! Halting.");
            // memset(psram_buf, 0, fw_size);
            while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
        }
        
        // Hash over flash/PSRAM
        bl_hash_t hash_obj;
        // In this implementation, blsys_flash_read maps directly to PSRAM.
        if (!blsect_hash_over_flash(main_hdr, (bl_addr_t)(psram_buf + sizeof(bl_section_t)), &hash_obj, 0)) {
            ESP_LOGE(TAG, "Hash calculation failed.");
            while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
        }
        
        bl_section_t *sig_hdr = (bl_section_t *)(psram_buf + sizeof(bl_section_t) + main_hdr->pl_size);
        if (sig_hdr->magic == BL_SECT_MAGIC && blsect_is_signature(sig_hdr)) {
            uint8_t sig_msg[91];
            size_t sig_msg_size = sizeof(sig_msg);
            if (blsect_make_signature_message(sig_msg, &sig_msg_size, &hash_obj, 1)) {
                ESP_LOGI(TAG, "Performing secp256k1 multisig verification...");
                
                bl_set_progress_callback(crypto_progress_cb, NULL);
                
                int32_t sig_res = blsig_verify_multisig("secp256k1-sha256", 
                    (uint8_t*)sig_hdr + sizeof(bl_section_t), sig_hdr->pl_size, 
                    pubkeys_boot, sig_msg, sig_msg_size, 0);
                    
                if (blsig_is_error(sig_res)) {
                    ESP_LOGE(TAG, "Signature verification failed: %s", blsig_error_text(sig_res));
                    ESP_LOGE(TAG, "HALTING execution.");
                    memset(psram_buf, 0, fw_size);
                    while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
                } else {
                    ESP_LOGI(TAG, "Signature verification PASSED!");
                    bootloader_dual_print("Signature verification PASSED!\r\n");
                }
            }
        } else {
            ESP_LOGE(TAG, "Signature section missing. pl_size: %lu, sizeof(bl_section_t): %u, sig_magic: 0x%08lX", (unsigned long)main_hdr->pl_size, (unsigned int)sizeof(bl_section_t), (unsigned long)sig_hdr->magic);
            memset(psram_buf, 0, fw_size);
            while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
        }
        payload_offset = sizeof(bl_section_t);
        main_pl_addr += sizeof(bl_section_t);
    } else {
        ESP_LOGE(TAG, "Invalid image magic or missing Specter header. Halting.");
        memset(psram_buf, 0, fw_size);
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }

    ESP_LOGI(TAG, "Parsing application image header...");
    custom_esp_image_header_t *img_hdr = (custom_esp_image_header_t *)main_pl_addr;
    header_copy = *img_hdr; // copy it so we can free psram_buf
    
    if (header_copy.magic != 0xE9) {
        ESP_LOGE(TAG, "Invalid image magic: 0x%02lX.", (unsigned long)header_copy.magic);
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }

    ESP_LOGI(TAG, "Image segments: %d, Entry: 0x%08lX", header_copy.segment_count, (unsigned long)header_copy.entry_addr);

    if (header_copy.segment_count > 16) {
        ESP_LOGE(TAG, "Too many segments (%d > 16). Halting.", header_copy.segment_count);
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }

    uint32_t max_offset = 0;
    uint32_t offset = sizeof(custom_esp_image_header_t);
    for (int i = 0; i < header_copy.segment_count; i++) {
        custom_esp_image_segment_header_t seg_hdr;
        memcpy(&seg_hdr, (void*)(main_pl_addr + offset), sizeof(seg_hdr));
        offset += sizeof(custom_esp_image_segment_header_t);
        
        if (seg_hdr.data_len > MAX_FIRMWARE_SIZE) {
            ESP_LOGE(TAG, "Invalid segment length (%lu). Halting.", (unsigned long)seg_hdr.data_len);
            while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
        }

        // ESP32-P4: If payload uses PSRAM (0x48000000 range), it's a mapped segment.
        if (seg_hdr.load_addr >= 0x48000000 && seg_hdr.load_addr < 0x4C000000) {
            uint32_t end_offset = (seg_hdr.load_addr - 0x48000000) + seg_hdr.data_len;
            if (end_offset > max_offset) max_offset = end_offset;
        }
        offset += seg_hdr.data_len;
    }

    max_offset = (max_offset + 0xFFFF) & ~0xFFFF;
    ESP_LOGI(TAG, "Required PSRAM MMU footprint: %lu bytes", (unsigned long)max_offset);

    uint8_t *fake_flash = NULL;
    uint32_t flash_paddr = 0;
    
    if (max_offset > 0) {
        // We allocate a fake flash buffer in PSRAM and map that!
        fake_flash = heap_caps_aligned_alloc(65536, max_offset, MALLOC_CAP_SPIRAM);
        if (!fake_flash) {
            ESP_LOGE(TAG, "Failed to allocate fake_flash");
            while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
        }
        memset(fake_flash, 0, max_offset);
        
        mmu_target_t target;
        if (esp_mmu_vaddr_to_paddr(fake_flash, &flash_paddr, &target) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get paddr of fake_flash");
            while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
        }
        ESP_LOGI(TAG, "fake_flash vaddr: %p, paddr: 0x%08lX", fake_flash, (unsigned long)flash_paddr);
    }

    offset = sizeof(custom_esp_image_header_t);
    mmu_mapping_t mmu_mappings[16];
    int mapping_count = 0;
    uint32_t last_page_start_addr = 0xFFFFFFFF;

    for (int i = 0; i < header_copy.segment_count; i++) {
        custom_esp_image_segment_header_t seg_hdr;
        memcpy(&seg_hdr, (void*)(main_pl_addr + offset), sizeof(seg_hdr));
        offset += sizeof(custom_esp_image_segment_header_t);
        
        ESP_LOGI(TAG, "Loading segment %d: addr=0x%08lX, len=%lu", i, (unsigned long)seg_hdr.load_addr, (unsigned long)seg_hdr.data_len);
        
        if (seg_hdr.load_addr >= 0x48000000 && seg_hdr.load_addr < 0x4C000000) {
            // Mapped segment in PSRAM
            ESP_LOGI(TAG, "Mapping segment %d: vaddr=0x%lx, paddr_offset=0x%lx, len=%lu", i, 
                (unsigned long)seg_hdr.load_addr, (unsigned long)(seg_hdr.load_addr - 0x48000000), (unsigned long)seg_hdr.data_len);
            
            uint32_t aligned_start = seg_hdr.load_addr & ~0xFFFF;
            uint32_t aligned_end = (seg_hdr.load_addr + seg_hdr.data_len + 0xFFFF - 1) & ~0xFFFF;
            if (seg_hdr.data_len == 0) aligned_end = aligned_start;
            
            mmu_mappings[mapping_count].vaddr = aligned_start;
            mmu_mappings[mapping_count].len = aligned_end - aligned_start;
            mmu_mappings[mapping_count].paddr = flash_paddr + (aligned_start - 0x48000000);
            
            uint32_t write_addr_offset = (seg_hdr.load_addr - 0x48000000);
            
            // Just memcpy to fake_flash
            memcpy(fake_flash + write_addr_offset, (void*)(main_pl_addr + offset), seg_hdr.data_len);
            
            uint32_t page_start_offset = write_addr_offset & ~0xFFFF;
            if (page_start_offset != last_page_start_addr) {
                memcpy(fake_flash + page_start_offset, (void*)main_pl_addr, 32);
                last_page_start_addr = page_start_offset;
            }
            mapping_count++;
        } else {
            // Unmapped segments (HP L2MEM IRAM/DRAM, LP SRAM, etc.) — copy directly
            ESP_LOGI(TAG, "Queueing segment %d for copy: addr=0x%lx, len=%lu", i, (unsigned long)seg_hdr.load_addr, (unsigned long)seg_hdr.data_len);
            safe_copies[safe_copy_count].dest = (void *)seg_hdr.load_addr;
            safe_copies[safe_copy_count].src = (void *)(psram_buf + payload_offset + offset);
            safe_copies[safe_copy_count].len = seg_hdr.data_len;
            safe_copy_count++;
        }
        offset += seg_hdr.data_len;
    }

    Cache_WriteBack_Addr(0x10, (uint32_t)fake_flash, max_offset);
    Cache_WriteBack_Addr(0x20, (uint32_t)fake_flash, max_offset);

    // NOTE: Do NOT zero/free psram_buf here! The deferred copies in
    // do_mmu_mapping_and_jump() read from src pointers inside this buffer.
    // Since that function is __attribute__((noreturn)), cleanup is unnecessary.

    ESP_LOGI(TAG, "Jumping to entry point...");
    
    for (int i = 0; i < safe_copy_count; i++) {
        ESP_LOGI(TAG, "Copy %d: dest=%p, src=%p, len=%lu", i, safe_copies[i].dest, safe_copies[i].src, (unsigned long)safe_copies[i].len);
    }
    
    safe_entry_addr = header_copy.entry_addr;
    
    // Copy mmu_mappings into safe_mappings
    for (int i = 0; i < mapping_count; i++) {
        safe_mappings[i] = mmu_mappings[i];
    }
    safe_mapping_count = mapping_count;

    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    do_mmu_mapping_and_jump();
}
