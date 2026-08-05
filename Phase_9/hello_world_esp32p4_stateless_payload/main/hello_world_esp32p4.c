#include "esp_rom_sys.h"

void my_entry_point(void) {
    esp_rom_printf("\r\n\r\n========================================\r\n");
    esp_rom_printf("HELLO FROM BARE METAL PAYLOAD!!!\r\n");
    esp_rom_printf("========================================\r\n\r\n");
    while(1) {
        for (volatile int i = 0; i < 5000000; i++) {}
        esp_rom_printf("Still alive in payload...\r\n");
    }
}

void app_main(void) {}
