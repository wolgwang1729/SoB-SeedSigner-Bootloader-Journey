set(IDF_TARGET esp32s3)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.240mhz
    boards/sdkconfig.spiram_sx
    boards/sdkconfig.spiram_oct
    boards/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/sdkconfig.board
)

# Insert board hardware sdkconfig from esp-board-common before sdkconfig.board,
# so MicroPython-specific settings in sdkconfig.board take precedence.
if(DEFINED BOARD_CONFIG_DIR AND EXISTS "${BOARD_CONFIG_DIR}/sdkconfig.defaults")
    list(INSERT SDKCONFIG_DEFAULTS 4 "${BOARD_CONFIG_DIR}/sdkconfig.defaults")
endif()

# Append stateless shim component for custom bootloader execution
list(APPEND MICROPY_EXTRA_COMPONENT_DIRS "${MICROPY_BOARD_DIR}/stateless_shim")
