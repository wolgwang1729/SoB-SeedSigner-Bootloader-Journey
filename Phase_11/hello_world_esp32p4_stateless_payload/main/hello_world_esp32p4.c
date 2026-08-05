#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

extern void call_start_cpu0(void);

// A simple spin-wait to drain the UART TX FIFO before hardware reconfiguration wipes it
static void drain_uart_fifo(void) {
    for(volatile int i=0; i<5000000; i++);
}

void my_entry_point(void) {
    esp_rom_printf("\r\n--- ENTERED my_entry_point! ---\r\n");

    esp_rom_printf("Invalidating I-Cache via ROM...\r\n");
    extern int Cache_Invalidate_All(uint32_t map);
    Cache_Invalidate_All(0x03);
    asm volatile ("fence.i\n");

    esp_rom_printf("Attempting to jump to call_start_cpu0...\r\n");
    // Wait for FIFO to flush before jumping to OS startup, as system_early_init 
    // will soon reconfigure clocks and wipe the UART buffer.
    drain_uart_fifo();
    call_start_cpu0();
    
    while (1);
}

// ---------------------------------------------------------
// BRICK 4: THE "RUG PULL" INTERCEPTORS
// We use GNU Linker --wrap feature to intercept these and
// preserve our bootloader's hardware/MMU configuration!
// ---------------------------------------------------------

void __wrap_cache_hal_init(void) {
    esp_rom_printf("[Intercepted] cache_hal_init() - preserving cache!\r\n");
    drain_uart_fifo();
}

void __wrap_esp_mmu_map_init(void) {
    esp_rom_printf("[Intercepted] esp_mmu_map_init() - preserving MMU!\r\n");
    drain_uart_fifo();
}

void __wrap_spi_flash_init_chip_state(void) {
    esp_rom_printf("[Intercepted] spi_flash_init_chip_state() - preserving SPI!\r\n");
    drain_uart_fifo();
}

void __wrap_mspi_timing_flash_tuning(void) {
    esp_rom_printf("[Intercepted] mspi_timing_flash_tuning() - bypassing tuning!\r\n");
    drain_uart_fifo();
}

int __wrap_esp_psram_chip_init(void) {
    esp_rom_printf("[Intercepted] esp_psram_chip_init() - bypassing hardware reset!\r\n");
    drain_uart_fifo();
    return 0; 
}

int __wrap_esp_psram_init(void) {
    esp_rom_printf("[Intercepted] esp_psram_init() - bypassing OS init!\r\n");
    drain_uart_fifo();
    return 0; 
}

void __wrap_bootloader_flash_update_id(void) {
    esp_rom_printf("[Intercepted] bootloader_flash_update_id() - preserving ID!\r\n");
    drain_uart_fifo();
}

int __wrap_image_process(void) {
    esp_rom_printf("[Intercepted] image_process() - bypassing flash image loading!\r\n");
    drain_uart_fifo();
    return 0; 
}

void __wrap_esp_mspi_pin_init(void) {
    esp_rom_printf("[Intercepted] esp_mspi_pin_init() - preserving pins!\r\n");
    drain_uart_fifo();
}

void __wrap_esp_mspi_pin_reserve(void) {
    esp_rom_printf("[Intercepted] esp_mspi_pin_reserve() - preserving pins!\r\n");
    drain_uart_fifo();
}

void __wrap_sys_rtc_init(void *rst_reas) {
    esp_rom_printf("[Intercepted] sys_rtc_init() called!\r\n");
    drain_uart_fifo();
    extern void __real_sys_rtc_init(void *rst_reas);
    __real_sys_rtc_init(rst_reas);
}

void __wrap_system_early_init(void *rst_reas) {
    esp_rom_printf("[Intercepted] system_early_init() called!\r\n");
    drain_uart_fifo();
    extern void __real_system_early_init(void *rst_reas);
    __real_system_early_init(rst_reas);
}

void app_main(void) {
    esp_rom_printf("\r\n\r\n========================================\r\n");
    esp_rom_printf("PHASE 11: STATELESS PAYLOAD ON APP LOADER\r\n");
    esp_rom_printf("========================================\r\n\r\n");
    
    while(1) {
        esp_rom_printf("Hello from a FreeRTOS Task running completely in PSRAM!\r\n");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
