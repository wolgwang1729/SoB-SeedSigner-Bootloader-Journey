#include <stdio.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#define MOUNT_POINT "/sdcard"

void test_sdcard(void) {
    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    
    printf("\nInitializing SD card via SDMMC...\n");
    
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // ESP32-P4 requires on-chip LDO to power the SD card IO.
    // LDO channel 4 is the standard for SDMMC on ESP32-P4.
    printf("Enabling on-chip LDO power for SD card (channel 4)...\n");
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;

    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        printf("Failed to create on-chip LDO power control driver (error: %s)\n", esp_err_to_name(ret));
        return;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4; // Use 4-bit mode
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    // Waveshare ESP32-P4-Module-DEV-KIT pins (SDMMC Slot 1 via GPIO matrix)
    slot_config.clk = 43;
    slot_config.cmd = 44;
    slot_config.d0 = 39;
    slot_config.d1 = 40;
    slot_config.d2 = 41;
    slot_config.d3 = 42;
    
    printf("Mounting FAT filesystem...\n");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            printf("Failed to mount filesystem. (Error: ESP_FAIL)\n");
        } else {
            printf("Failed to initialize the card (error code: %s).\n", esp_err_to_name(ret));
        }
        return;
    }
    
    printf("SD Card mounted successfully!\n");
    sdmmc_card_print_info(stdout, card);
    
    // Test reading a file
    printf("Opening /sdcard/hello.txt...\n");
    FILE *f = fopen("/sdcard/hello.txt", "r");
    if (f == NULL) {
        printf("Failed to open hello.txt for reading\n");
    } else {
        char line[128];
        if (fgets(line, sizeof(line), f) != NULL) {
            printf("Read from file: '%s'\n", line);
        } else {
            printf("File is empty or could not be read.\n");
        }
        fclose(f);
    }
    
    // Unmount
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    printf("SD Card unmounted cleanly.\n");

    // Cleanup LDO
    sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle);
}
