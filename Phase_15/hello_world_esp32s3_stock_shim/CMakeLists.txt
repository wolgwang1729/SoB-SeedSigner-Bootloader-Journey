# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# NOTE: MINIMAL_BUILD (present in the stock example) is intentionally removed:
# it drops esp_psram from the component set, so the CONFIG_SPIRAM_* defaults in
# sdkconfig.defaults are silently discarded and the app's IROM/DROM stay in the
# 0x40000000 flash window instead of 0x48000000 (which the loader MMU-maps to
# PSRAM). A full component set keeps the PSRAM XIP memory layout in place.
project(hello_world)
