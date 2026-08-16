#include "board.h"
#include "board_config.h"

#if BOARD_TOUCH_DRIVER == TOUCH_AXS15231B

#include "board_touch_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_axs15231b.h"
#include "esp_log.h"

static const char *TAG = "touch_axs15231b";

esp_lcd_touch_handle_t board_touch_axs15231b_init(i2c_master_bus_handle_t bus,
                                                    uint16_t x_max, uint16_t y_max)
{
    ESP_LOGI(TAG, "Skipping AXS15231B touch init (no screen/touch connected / headless mode)");
    return NULL;
}

#endif /* BOARD_TOUCH_DRIVER == TOUCH_AXS15231B */
