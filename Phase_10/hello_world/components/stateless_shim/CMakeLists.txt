idf_component_register(SRCS "stateless_shim.c"
                       INCLUDE_DIRS ".")

# Force the linker to pull in our custom entry point and __wrap_ symbols
target_link_options(${COMPONENT_LIB} INTERFACE
    "-u__wrap_call_start_cpu0"
    "-Wl,-e,my_entry_point"
)
