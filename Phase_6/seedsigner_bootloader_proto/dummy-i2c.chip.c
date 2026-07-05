#include "wokwi-api.h"

static bool on_i2c_connect(void *user_data, uint32_t address, bool read) { return true; }
static uint8_t on_i2c_read(void *user_data, uint32_t i2c_index) { return 0x00; }
static bool on_i2c_write(void *user_data, uint32_t i2c_index, uint8_t data) { return true; }

void chip_init(void) {
    pin_t sda = pin_init("SDA", INPUT_PULLUP);
    pin_t scl = pin_init("SCL", INPUT_PULLUP);
    
    // 6 addresses to keep memory footprint low but cover all Waveshare 3.5B onboard devices
    uint32_t addresses[] = {
        0x20, // TCA9554 IO Expander (primary)
        0x38, // TCA9554 IO Expander (alternate)
        0x34, // AXP2101 PMU
        0x3B, // AXS15231B Touch
        0x51, // PCF85063 RTC
        0x6B  // QMI8658 IMU
    };
    
    for (int i = 0; i < 6; i++) {
        i2c_config_t cfg = {
            .address = addresses[i],
            .sda = sda,
            .scl = scl,
            .connect = on_i2c_connect,
            .read = on_i2c_read,
            .write = on_i2c_write,
        };
        i2c_init(&cfg);
    }
}
