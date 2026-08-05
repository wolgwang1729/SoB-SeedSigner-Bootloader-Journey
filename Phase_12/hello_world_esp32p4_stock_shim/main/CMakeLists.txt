idf_component_register(SRCS "hello_world_main.c"
                       REQUIRES stateless_shim
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "")
