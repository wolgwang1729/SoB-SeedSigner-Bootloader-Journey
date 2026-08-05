idf_component_register(SRCS "hello_world_esp32p4.c" "sdcard_test.c"
                    INCLUDE_DIRS "."
                    REQUIRES fatfs sdmmc)
target_link_options(${COMPONENT_LIB} INTERFACE 
    "-u__wrap_call_start_cpu0"
    "-Wl,-e,my_entry_point"
)
