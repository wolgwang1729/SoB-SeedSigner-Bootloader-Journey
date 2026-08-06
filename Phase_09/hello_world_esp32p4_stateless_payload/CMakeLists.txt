# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello_world_esp32p4_stateless_payload)

target_link_libraries(${CMAKE_PROJECT_NAME}.elf PRIVATE
    "-Wl,--wrap=call_start_cpu0"
)
