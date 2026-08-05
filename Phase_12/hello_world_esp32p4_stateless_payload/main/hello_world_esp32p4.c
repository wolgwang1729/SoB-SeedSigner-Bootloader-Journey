#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

// Stateless-shim test payload for Phase 12.
//
// Boot flow: Phase 12 loader verifies the Specter-signed bundle → JMP sequence
// → my_entry_point (components/stateless_shim) → __wrap_call_start_cpu0 hand-off
// (bss, clocks, MMU, init fns, PSRAM heap) → esp_startup_start_app → app_main.
// All code executes from PSRAM; internal-SRAM segments were copied by the loader.

void app_main(void) {
    esp_rom_printf("\r\n\r\n========================================\r\n");
    esp_rom_printf("PHASE 12: SHIM-BASED STATELESS PAYLOAD\r\n");
    esp_rom_printf("========================================\r\n\r\n");

    while (1) {
        esp_rom_printf("Hello from a FreeRTOS Task running completely in PSRAM (shim boot)!\r\n");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
