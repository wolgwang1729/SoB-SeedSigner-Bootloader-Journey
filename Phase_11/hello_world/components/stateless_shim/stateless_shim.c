/*
 * Stateless PSRAM Shim for SeedSigner Bootloader
 * 
 * This component intercepts ESP-IDF hardware initialization functions
 * that would otherwise destroy the MMU/Cache/Flash state set up by
 * the seedsigner bootloader. It also provides a custom entry point
 * that invalidates the I-Cache before jumping into call_start_cpu0.
 *
 * Drop this component into any ESP-IDF project to make it bootable
 * via the stateless PSRAM bootloader. NO changes to app_main needed.
 */

#include "esp_rom_sys.h"

extern void call_start_cpu0(void);

/* Custom entry point — the bootloader jumps here */
void my_entry_point(void) {
    esp_rom_printf("\r\n--- ENTERED my_entry_point! ---\r\n");

    /* Invalidate I-Cache so the CPU fetches fresh instructions
       from SRAM instead of stale ones left by the ROM bootloader */
    esp_rom_printf("Invalidating I-Cache via ROM...\r\n");
    extern int Cache_Invalidate_All(uint32_t map);
    Cache_Invalidate_All(0x03);  /* CACHE_L1_ICACHE0 | CACHE_L1_ICACHE1 */
    asm volatile ("fence.i\n");

    esp_rom_printf("Jumping to call_start_cpu0...\r\n");
    call_start_cpu0();

    while (1); /* Should never reach here */
}

/* ---- Hardware Interceptors (--wrap stubs) ---- */
/* These prevent call_start_cpu0 from resetting hardware state */

void __wrap_cache_hal_init(void) {
    esp_rom_printf("[Intercepted] cache_hal_init() - preserving cache!\r\n");
}

void __wrap_esp_mmu_map_init(void) {
    esp_rom_printf("[Intercepted] esp_mmu_map_init() - preserving MMU!\r\n");
}

void __wrap_spi_flash_init_chip_state(void) {
    esp_rom_printf("[Intercepted] spi_flash_init_chip_state() - preserving SPI!\r\n");
}

void __wrap_mspi_timing_flash_tuning(void) {
    esp_rom_printf("[Intercepted] mspi_timing_flash_tuning() - bypassing tuning!\r\n");
}

int __wrap_esp_psram_chip_init(void) {
    esp_rom_printf("[Intercepted] esp_psram_chip_init() - bypassing hardware reset!\r\n");
    return 0;
}

int __wrap_esp_psram_init(void) {
    esp_rom_printf("[Intercepted] esp_psram_init() - bypassing OS init!\r\n");
    return 0;
}

void __wrap_bootloader_flash_update_id(void) {
    esp_rom_printf("[Intercepted] bootloader_flash_update_id() - preserving ID!\r\n");
}

int __wrap_image_process(void) {
    esp_rom_printf("[Intercepted] image_process() - bypassing flash image loading!\r\n");
    return 0;
}

void __wrap_esp_mspi_pin_init(void) {
    esp_rom_printf("[Intercepted] esp_mspi_pin_init() - preserving pins!\r\n");
}

void __wrap_esp_mspi_pin_reserve(void) {
    esp_rom_printf("[Intercepted] esp_mspi_pin_reserve() - preserving pins!\r\n");
}

void __wrap_sys_rtc_init(void *rst_reas) {
    esp_rom_printf("[Intercepted] sys_rtc_init() called!\r\n");
    extern void __real_sys_rtc_init(void *rst_reas);
    __real_sys_rtc_init(rst_reas);
}

void __wrap_system_early_init(void *rst_reas) {
    esp_rom_printf("[Intercepted] system_early_init() called!\r\n");
    extern void __real_system_early_init(void *rst_reas);
    __real_system_early_init(rst_reas);
}
