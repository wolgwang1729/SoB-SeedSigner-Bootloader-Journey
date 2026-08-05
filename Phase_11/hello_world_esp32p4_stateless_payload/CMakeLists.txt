cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello_world_esp32p4_stateless_payload)

target_link_libraries(${CMAKE_PROJECT_NAME}.elf PRIVATE
    "-Wl,-e,my_entry_point"
    "-Wl,--wrap=cache_hal_init"
    "-Wl,--wrap=esp_mmu_map_init"
    "-Wl,--wrap=spi_flash_init_chip_state"
    "-Wl,--wrap=mspi_timing_flash_tuning"
    "-Wl,--wrap=esp_psram_chip_init"
    "-Wl,--wrap=esp_psram_init"
    "-Wl,--wrap=bootloader_flash_update_id"
    "-Wl,--wrap=image_process"
    "-Wl,--wrap=esp_mspi_pin_init"
    "-Wl,--wrap=esp_mspi_pin_reserve"
    "-Wl,--wrap=sys_rtc_init"
    "-Wl,--wrap=system_early_init"
)
