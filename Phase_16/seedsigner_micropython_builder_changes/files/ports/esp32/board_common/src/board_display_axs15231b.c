#include "board.h"
#include "board_config.h"

#if BOARD_DISPLAY_DRIVER == DISPLAY_AXS15231B

#include "board_display_axs15231b.h"
#include "esp_log.h"

static const char *TAG = "display_axs15231b";

void board_display_axs15231b_init(esp_lcd_panel_io_handle_t *io_handle,
                                   esp_lcd_panel_handle_t *panel_handle,
                                   size_t max_transfer_sz)
{
    ESP_LOGI(TAG, "Skipping AXS15231B display init (no screen connected / headless mode)");
    if (io_handle) *io_handle = NULL;
    if (panel_handle) *panel_handle = NULL;
    return;
}

#endif /* BOARD_DISPLAY_DRIVER == DISPLAY_AXS15231B */
