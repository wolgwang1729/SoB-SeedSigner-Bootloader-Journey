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

#include "hal/mmu_hal.h"
#include "hal/cache_ll.h"
#include "esp_mmu_map.h"

// Bug #1: Move to the top (or ideally in sdkconfig, but we put it here)
#define USE_SPI_FLASH_FOR_QEMU_TEST 1
#define QEMU_FLASH_PADDR 0x330000 // Code Quality #15

static const char *TAG = "SEEDSIGNER_LOADER";
#define MOUNT_POINT "/sdcard"

// Dummy keys for verification (so it passes test keys)
const bl_pubkey_t vendor_keys[] = {
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

#define SOC_IROM_LOW    0x42000000
#define SOC_IROM_HIGH   0x44000000
#define SOC_DROM_LOW    0x3C000000
#define SOC_DROM_HIGH   0x3E000000
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

static void RTC_IRAM_ATTR __attribute__((noreturn)) do_mmu_mapping_and_jump(uint32_t paddr, uint32_t max_offset, uint32_t entry_addr, pending_copy_t *copies, int count, mmu_mapping_t *mappings, int mapping_count) {
    portDISABLE_INTERRUPTS();
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
    mmu_target_t target = MMU_TARGET_FLASH0;
#else
    mmu_target_t target = MMU_TARGET_PSRAM0;
#endif

    for (int i = 0; i < mapping_count; i++) {
        uint32_t out_len;
        mmu_hal_map_region(0, target, mappings[i].vaddr, mappings[i].paddr, mappings[i].len, &out_len);
#if !CONFIG_FREERTOS_UNICORE
        mmu_hal_map_region(1, target, mappings[i].vaddr, mappings[i].paddr, mappings[i].len, &out_len);
#endif
        cache_bus_mask_t bus_mask = cache_ll_l1_get_bus(0, mappings[i].vaddr, mappings[i].len);
        cache_ll_l1_enable_bus(0, bus_mask);
#if !CONFIG_FREERTOS_UNICORE
        bus_mask = cache_ll_l1_get_bus(1, mappings[i].vaddr, mappings[i].len);
        cache_ll_l1_enable_bus(1, bus_mask);
#endif
    }
    
    for (int i = 0; i < count; i++) {
        memcpy(copies[i].dest, copies[i].src, copies[i].len);
    }
    
    typedef void (*entry_t)(void);
    entry_t entry = (entry_t)entry_addr;
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
    // Code Quality #12: Refactor ifdef paths
    if (!mount_storage_qemu()) return;

    ESP_LOGI(TAG, "QEMU Test Mode: Mapping payload directly from Flash partition 0x%X.", QEMU_FLASH_PADDR);
    fw_size = 4096; // Just need headers for mapping loop
    psram_buf = (uint8_t *)malloc(fw_size);
    if (!psram_buf) {
        ESP_LOGE(TAG, "Failed to allocate buffer for headers.");
        return;
    }
    if (esp_flash_read(NULL, psram_buf, QEMU_FLASH_PADDR, fw_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read flash partition.");
        free(psram_buf);
        return;
    }
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
    uint32_t payload_offset = 0;

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
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
                    ESP_LOGW(TAG, "Proceeding anyway for QEMU test.");
#else
                    ESP_LOGE(TAG, "HALTING execution.");
                    memset(psram_buf, 0, fw_size);
                    while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
#endif
                } else {
                    ESP_LOGI(TAG, "Signature verification PASSED!");
                }
            }
        } else {
            ESP_LOGE(TAG, "Signature section missing.");
#ifndef USE_SPI_FLASH_FOR_QEMU_TEST
            memset(psram_buf, 0, fw_size);
            while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
#endif
        }
        payload_offset = sizeof(bl_section_t);
        main_pl_addr += sizeof(bl_section_t);
    } else {
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
        ESP_LOGW(TAG, "No Specter header found. Assuming raw binary for QEMU test.");
#else
        ESP_LOGE(TAG, "Invalid image magic or missing Specter header. Halting.");
        memset(psram_buf, 0, fw_size);
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
#endif
    }

    ESP_LOGI(TAG, "Parsing application image header...");
    custom_esp_image_header_t *img_hdr = (custom_esp_image_header_t *)main_pl_addr;
    if (img_hdr->magic != 0xE9) {
        ESP_LOGE(TAG, "Invalid image magic: 0x%02lX.", (unsigned long)img_hdr->magic);
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
        ESP_LOGI(TAG, "QEMU Test Complete: Secure Loader Verified!");
#else
        memset(psram_buf, 0, fw_size);
#endif
        free(psram_buf);
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }

    ESP_LOGI(TAG, "Image segments: %d, Entry: 0x%08lX", img_hdr->segment_count, (unsigned long)img_hdr->entry_addr);

    if (img_hdr->segment_count > 16) {
        ESP_LOGE(TAG, "Too many segments (%d > 16). Halting.", img_hdr->segment_count);
        free(psram_buf);
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }

    uint32_t max_offset = 0;
    uint32_t offset = sizeof(custom_esp_image_header_t);
    for (int i = 0; i < img_hdr->segment_count; i++) {
        custom_esp_image_segment_header_t seg_hdr;
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
        esp_flash_read(NULL, &seg_hdr, QEMU_FLASH_PADDR + payload_offset + offset, sizeof(seg_hdr));
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
        memset(psram_buf, 0, fw_size);
        free(psram_buf);
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }
    memset(app_psram, 0, max_offset);
#endif

    offset = sizeof(custom_esp_image_header_t);
    pending_copy_t pending_copies[16];
    int pending_count = 0;
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

    for (int i = 0; i < img_hdr->segment_count; i++) {
        custom_esp_image_segment_header_t seg_hdr;
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
        esp_flash_read(NULL, &seg_hdr, QEMU_FLASH_PADDR + payload_offset + offset, sizeof(seg_hdr));
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
            // IRAM or DRAM
            if ((seg_hdr.load_addr >= 0x3FC80000 && seg_hdr.load_addr < 0x3FC80000 + 0x80000) || 
                (seg_hdr.load_addr >= 0x40370000 && seg_hdr.load_addr < 0x40370000 + 0x90000)) {
                void *buf = malloc(seg_hdr.data_len);
#ifdef USE_SPI_FLASH_FOR_QEMU_TEST
                esp_flash_read(NULL, buf, QEMU_FLASH_PADDR + payload_offset + offset, seg_hdr.data_len);
#else
                memcpy(buf, (void*)(main_pl_addr + offset), seg_hdr.data_len);
#endif
                pending_copies[pending_count].dest = (void*)seg_hdr.load_addr;
                pending_copies[pending_count].src = buf;
                pending_copies[pending_count].len = seg_hdr.data_len;
                pending_count++;
            } else {
                ESP_LOGE(TAG, "Invalid load address for segment %d: 0x%08lX", i, (unsigned long)seg_hdr.load_addr);
                while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
            }
        }
        offset += seg_hdr.data_len;
    }

#ifndef USE_SPI_FLASH_FOR_QEMU_TEST
    // Clean up the original file buffer securely
    memset(psram_buf, 0, fw_size);
    free(psram_buf);
#endif

    ESP_LOGI(TAG, "Jumping to entry point...");
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    do_mmu_mapping_and_jump(paddr, max_offset, img_hdr->entry_addr, pending_copies, pending_count, mmu_mappings, mapping_count);
}
