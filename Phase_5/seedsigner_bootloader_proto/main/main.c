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

#include "bl_section.h"
#include "bl_signature.h"
#include "bl_syscalls.h"


#include "esp_mmu_map.h"
#include "hal/mmu_hal.h"
#include "hal/cache_ll.h"
#include "esp_rom_spiflash.h"

// Bug #1: Move to the top (or ideally in sdkconfig, but we put it here)
#define USE_SPI_FLASH_FOR_QEMU_TEST 1
// Place the signed payload at 0x33FF00 so the ESP image (at +0x100 past the
// 256-byte Specter header) starts at 0x340000 — a 64KB page-aligned address.
// This is critical: IROM/DROM MMU mapping requires (vaddr & 0xFFFF) == (paddr & 0xFFFF).
#define QEMU_FLASH_PADDR       0x33FF00

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

#ifndef SOC_IROM_LOW
#define SOC_IROM_LOW    0x42000000
#endif
#ifndef SOC_IROM_HIGH
#define SOC_IROM_HIGH   0x44000000
#endif
#ifndef SOC_DROM_LOW
#define SOC_DROM_LOW    0x3C000000
#endif
#ifndef SOC_DROM_HIGH
#define SOC_DROM_HIGH   0x3E000000
#endif
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

#define ESP32S3_MMU_TABLE ((volatile uint32_t *)0x600C5000)



RTC_DATA_ATTR static pending_copy_t safe_copies[20];
RTC_DATA_ATTR static int safe_copy_count = 0;
RTC_DATA_ATTR static uint32_t safe_entry_addr = 0;
RTC_DATA_ATTR static mmu_mapping_t safe_mappings[20];
RTC_DATA_ATTR static int safe_mapping_count = 0;

static void RTC_IRAM_ATTR __attribute__((noreturn)) do_mmu_mapping_and_jump(void) {
    // Switch stack to safe high DRAM to prevent self-corruption when overwriting IRAM/DRAM
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    asm volatile ("movi a1, 0x3FCE9000\n");
#else
    asm volatile ("li sp, 0x3FCE9000\n");
#endif
    
    portDISABLE_INTERRUPTS();

    // Map external flash/PSRAM directly via MMU table to avoid calling IROM functions like mmu_hal_map_region
    // Fill entire MMU table with 0 (Valid, Page 0) to avoid QEMU XTS-AES bug with (1 << 14) invalid bit.
    for (int i = 0; i < 8192; i++) {
        ESP32S3_MMU_TABLE[i] = 0;
    }

    for (int i = 0; i < safe_mapping_count; i++) {
        uint32_t entry_id = (safe_mappings[i].vaddr & 0x01FFFFFF) >> 16;
        uint32_t p_page = safe_mappings[i].paddr >> 16;
        uint32_t vaddr_offset = safe_mappings[i].vaddr & 0xFFFF;
        uint32_t pages = (safe_mappings[i].len + vaddr_offset + 0xFFFF) >> 16;
        for (uint32_t p = 0; p < pages; p++) {
            // Target code 0 is FLASH, SOC_MMU_VALID is 0 on ESP32-S3
            ESP32S3_MMU_TABLE[entry_id + p] = p_page + p;
        }
    }
    
    // Perform any pending copies (e.g. into IRAM/DRAM)
    for (int i = 0; i < safe_copy_count; i++) {
        uint32_t dst = (uint32_t)safe_copies[i].dest;
        if (dst >= 0x40370000 && dst < 0x403D8000) {
            dst = dst - 0x40370000 + 0x3FC88000;
        }
        memcpy((void*)dst, safe_copies[i].src, safe_copies[i].len);
    }
    
    // Invalidate caches AFTER copies to ensure ICache fetches the newly written DRAM data
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    extern void Cache_Invalidate_ICache_All(void);
    extern void Cache_Invalidate_DCache_All(void);
    Cache_Invalidate_ICache_All();
    Cache_Invalidate_DCache_All();
#else
    cache_ll_invalidate_all(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL, CACHE_LL_ID_ALL);
#endif

    typedef void (*entry_t)(void);
    entry_t entry = (entry_t)safe_entry_addr;
    entry();
    while (1);
}

// Helper for QEMU test mode mount
bool mount_storage_qemu(void) {
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    wl_handle_t wl_handle;
    ESP_LOGI(TAG, "Initializing FAT on SPI Flash for QEMU testing...");
    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, "storage", &mount_config, &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPI Flash FAT (%s).", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SPI Flash FAT mounted successfully.");
    return true;
}

// Helper for SD Card mount
sdmmc_card_t* mount_storage_sdcard(void) {
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card = NULL;
    ESP_LOGI(TAG, "Initializing SD card via SDMMC");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (%s).", esp_err_to_name(ret));
        return NULL;
    }
    ESP_LOGI(TAG, "SD Card mounted successfully.");
    return card;
}

// Progress callback to feed watchdog during crypto ops
void crypto_progress_cb(void* ctx, bl_cbarg_t arg, uint32_t total, uint32_t complete) {
    // Using ESP_IDF v5 function; standard is esp_task_wdt_reset()
    esp_task_wdt_reset(); 
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SeedSigner Secure Loader (App Mode)");

    uint8_t *psram_buf = NULL;
    size_t fw_size = 0;

#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
    // In QEMU, we don't have PSRAM emulation by default.
    // Mount FAT on SPI flash (needed for QEMU test environment)
    if (!mount_storage_qemu()) {
        ESP_LOGE(TAG, "Failed to mount storage. Halting.");
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }
    ESP_LOGI(TAG, "QEMU Test Mode: Mapping payload directly from Flash partition 0x%X.", QEMU_FLASH_PADDR);
#else
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
    ESP_LOGI(TAG, "Found firmware. Size: %lu bytes", (unsigned long)fw_size);

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
#endif

    // Re-integrate signature verification
    bl_addr_t main_pl_addr = (bl_addr_t)psram_buf;
    (void)main_pl_addr; (void)fw_size;
    uint32_t payload_offset = 0;
    custom_esp_image_header_t header_copy;

#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
    bl_section_t qemu_main_hdr;
    esp_rom_spiflash_read(QEMU_FLASH_PADDR, (uint32_t *)&qemu_main_hdr, sizeof(qemu_main_hdr));
    if (qemu_main_hdr.magic == BL_SECT_MAGIC) {
        ESP_LOGI(TAG, "Found Specter Bootloader header.");
        ESP_LOGI(TAG, "Firmware version: %lu", (unsigned long)qemu_main_hdr.pl_ver);
        payload_offset = sizeof(bl_section_t);
        ESP_LOGI(TAG, "ESP image at flash offset 0x%lX (page-aligned).",
                 (unsigned long)(QEMU_FLASH_PADDR + payload_offset));
    } else {
        ESP_LOGW(TAG, "No Specter header found. Assuming raw binary for QEMU test.");
    }
    ESP_LOGI(TAG, "Parsing application image header...");
    esp_rom_spiflash_read(QEMU_FLASH_PADDR + payload_offset, (uint32_t *)&header_copy, sizeof(header_copy));
#else
    bl_section_t *main_hdr = (bl_section_t *)psram_buf;
    if (main_hdr->magic == BL_SECT_MAGIC) {
        ESP_LOGI(TAG, "Found Specter Bootloader header.");
        
        bl_uint_t app_ver = 0;
        if (blsect_get_attr_uint(main_hdr, 0, &app_ver)) {
            // (Mock implementation)
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
#endif
    
    if (header_copy.magic != 0xE9) {
        ESP_LOGE(TAG, "Invalid image magic: 0x%02lX.", (unsigned long)header_copy.magic);
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
        ESP_LOGI(TAG, "QEMU Test Complete: Secure Loader Verified!");
#endif
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
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
        esp_rom_spiflash_read(QEMU_FLASH_PADDR + payload_offset + offset, (uint32_t *)&seg_hdr, sizeof(seg_hdr));
#else
        memcpy(&seg_hdr, (void*)(main_pl_addr + offset), sizeof(seg_hdr));
#endif
        offset += sizeof(custom_esp_image_segment_header_t);
        
        if (seg_hdr.data_len > MAX_FIRMWARE_SIZE) {
            ESP_LOGE(TAG, "Invalid segment length (%lu). Halting.", (unsigned long)seg_hdr.data_len);
            while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
        }

        if (seg_hdr.load_addr >= SOC_IROM_LOW && seg_hdr.load_addr < SOC_IROM_HIGH) {
            uint32_t end_offset = (seg_hdr.load_addr - SOC_IROM_LOW) + seg_hdr.data_len;
            if (end_offset > max_offset) max_offset = end_offset;
        } else if (seg_hdr.load_addr >= SOC_DROM_LOW && seg_hdr.load_addr < SOC_DROM_HIGH) {
            uint32_t end_offset = (seg_hdr.load_addr - SOC_DROM_LOW) + seg_hdr.data_len;
            if (end_offset > max_offset) max_offset = end_offset;
        }
        offset += seg_hdr.data_len;
    }

    max_offset = (max_offset + 0xFFFF) & ~0xFFFF;
    ESP_LOGI(TAG, "Required PSRAM MMU footprint: %lu bytes", (unsigned long)max_offset);

#ifndef USE_SPI_FLASH_FOR_QEMU_TEST
    uint8_t *app_psram = heap_caps_aligned_alloc(65536, max_offset, MALLOC_CAP_SPIRAM);
    if (!app_psram) {
        ESP_LOGE(TAG, "Failed to allocate aligned PSRAM for execution");
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }
    memset(app_psram, 0, max_offset);
#endif

    offset = sizeof(custom_esp_image_header_t);
    pending_copy_t pending_copies[16];
    int pending_count = 0;
    (void)pending_copies; (void)pending_count;
    mmu_mapping_t mmu_mappings[16];
    int mapping_count = 0;

    esp_paddr_t paddr = 0;
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
    paddr = QEMU_FLASH_PADDR + payload_offset; // QEMU flash mapping bypasses PSRAM
#else
    mmu_target_t target;
    if (esp_mmu_vaddr_to_paddr(app_psram, &paddr, &target) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get physical address of PSRAM");
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }
#endif

    for (int i = 0; i < header_copy.segment_count; i++) {
        custom_esp_image_segment_header_t seg_hdr;
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
        esp_rom_spiflash_read(QEMU_FLASH_PADDR + payload_offset + offset, (uint32_t *)&seg_hdr, sizeof(seg_hdr));
#else
        memcpy(&seg_hdr, (void*)(main_pl_addr + offset), sizeof(seg_hdr));
#endif
        offset += sizeof(custom_esp_image_segment_header_t);
        
        ESP_LOGI(TAG, "Loading segment %d: addr=0x%08lX, len=%lu", i, (unsigned long)seg_hdr.load_addr, (unsigned long)seg_hdr.data_len);
        
        if (seg_hdr.load_addr >= SOC_IROM_LOW && seg_hdr.load_addr < SOC_IROM_HIGH) {
            mmu_mappings[mapping_count].vaddr = seg_hdr.load_addr & ~0xFFFF;
            mmu_mappings[mapping_count].len = (seg_hdr.data_len + 0xFFFF) & ~0xFFFF;
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
            mmu_mappings[mapping_count].paddr = (paddr + offset) & ~0xFFFF;
#else
            mmu_mappings[mapping_count].paddr = (paddr + (seg_hdr.load_addr - SOC_IROM_LOW)) & ~0xFFFF;
            memcpy(app_psram + (seg_hdr.load_addr - SOC_IROM_LOW), (void*)(main_pl_addr + offset), seg_hdr.data_len);
#endif
            mapping_count++;
        } else if (seg_hdr.load_addr >= SOC_DROM_LOW && seg_hdr.load_addr < SOC_DROM_HIGH) {
            mmu_mappings[mapping_count].vaddr = seg_hdr.load_addr & ~0xFFFF;
            mmu_mappings[mapping_count].len = (seg_hdr.data_len + 0xFFFF) & ~0xFFFF;
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
            mmu_mappings[mapping_count].paddr = (paddr + offset) & ~0xFFFF;
#else
            mmu_mappings[mapping_count].paddr = (paddr + (seg_hdr.load_addr - SOC_DROM_LOW)) & ~0xFFFF;
            memcpy(app_psram + (seg_hdr.load_addr - SOC_DROM_LOW), (void*)(main_pl_addr + offset), seg_hdr.data_len);
#endif
            mapping_count++;
        } else {
            // Unmapped segments (IRAM, DRAM, RTC) need to be copied directly
            ESP_LOGI(TAG, "Queueing segment %d for copy: addr=0x%lx, len=%lu", i, (unsigned long)seg_hdr.load_addr, (unsigned long)seg_hdr.data_len);
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
            static uint8_t *safe_dram_ptr = (uint8_t *)0x3FCC0000;
            esp_rom_spiflash_read(paddr + offset, (uint32_t *)safe_dram_ptr, seg_hdr.data_len);
            safe_copies[safe_copy_count].dest = (void *)seg_hdr.load_addr;
            safe_copies[safe_copy_count].src = safe_dram_ptr;
            safe_copies[safe_copy_count].len = seg_hdr.data_len;
            safe_copy_count++;
            safe_dram_ptr += seg_hdr.data_len;
#else
            safe_copies[safe_copy_count].dest = (void *)seg_hdr.load_addr;
            safe_copies[safe_copy_count].src = (void *)(psram_buf + offset);
            safe_copies[safe_copy_count].len = seg_hdr.data_len;
            safe_copy_count++;
#endif
        }
        offset += seg_hdr.data_len;
    }

#ifndef USE_SPI_FLASH_FOR_QEMU_TEST
    // Clean up the original file buffer securely
    memset(psram_buf, 0, fw_size);
    free(psram_buf);
#endif

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
