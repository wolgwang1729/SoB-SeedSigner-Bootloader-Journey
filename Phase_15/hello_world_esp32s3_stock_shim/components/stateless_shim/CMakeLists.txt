idf_component_register(SRCS "stateless_shim.c" "entry.S"
                       INCLUDE_DIRS "."
                       REQUIRES esp_hw_support soc hal esp_rom heap esp_common esp_mm spi_flash)

# Force the linker to use our custom entry point (my_entry_point) so the image
# header entry_addr points at the shim's windowed-context setup instead of the
# stock call_start_cpu0. cache_hal_init is wrapped to a no-op as insurance:
# nothing in the app's later boot should re-run cache_init(), but if anything
# does, the hal context reset is harmless.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-u __wrap_cache_hal_init"
    "-Wl,-e,my_entry_point"
    "-Wl,--wrap=cache_hal_init"
)
