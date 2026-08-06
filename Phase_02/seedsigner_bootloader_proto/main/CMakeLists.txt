idf_component_register(SRCS "main.c" PRIV_REQUIRES specter_crypto fatfs esp_mm spi_flash nvs_flash)

# Create a FAT filesystem image from dummy_fat_dir and flash it with the project
fatfs_create_spiflash_image(storage ../dummy_fat_dir FLASH_IN_PROJECT)
