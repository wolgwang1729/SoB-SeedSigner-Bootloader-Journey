idf_component_register(SRCS "stateless_shim.c" "entry.S"
                       INCLUDE_DIRS "."
                       REQUIRES esp_hw_support soc hal esp_rom heap esp_common esp_mm spi_flash driver esp_driver_uart)

# Force custom entry point and wrapped functions
target_link_options(${COMPONENT_LIB} INTERFACE
    "-u __wrap_call_start_cpu0"
    "-u __wrap_cache_hal_init"
    "-u __wrap_uart_stdout_init"
    "-Wl,-e,my_entry_point"
    "-Wl,--wrap=cache_hal_init"
    "-Wl,--wrap=uart_stdout_init"
)
