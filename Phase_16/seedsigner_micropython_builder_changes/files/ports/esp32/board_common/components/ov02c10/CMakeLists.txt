# OV02C10 MIPI-CSI camera sensor driver, packaged as a standalone esp_cam_sensor
# add-on (OV02C10 is absent from the public espressif/esp_cam_sensor registry).
#
# The sensor source compiles only when CONFIG_CAMERA_OV02C10 is set, so boards
# that use a different sensor (e.g. the OV5647 Waveshare P4 targets) build an empty
# library and never register the ov02c10 detect function. This matters: OV02C10 and
# OV5647 share SCCB address 0x36, so only one may be auto-detectable per firmware.
#
# REQUIRES/PRIV_REQUIRES are declared UNCONDITIONALLY. ESP-IDF evaluates this file
# an extra time in script mode (early requirement collection) where CONFIG_* symbols
# do not exist; gating the whole idf_component_register() on CONFIG_CAMERA_OV02C10
# would make that pass see no requirements and drop the include paths (driver/gpio.h,
# esp_cam_sensor.h) from the real compile. Only SRCS/PRIV_INCLUDE_DIRS are gated —
# the same split esp_cam_sensor's own CMakeLists uses per sensor.
#
# esp_cam_sensor's linker fragment gathers the .esp_cam_sensor_detect_fn section
# from ALL archives (mapping: archive *), so a self-registered detect function in
# this separate component is picked up by esp_cam_sensor's auto-detect array. The
# "-u ov02c10_detect" keep prevents the linker from dropping this object (nothing
# else references it directly).

set(ov02c10_srcs "")
set(ov02c10_priv_inc "")
set(ov02c10_reqs "")
set(ov02c10_priv_reqs "")

if(IDF_TARGET STREQUAL "esp32p4")
    set(ov02c10_reqs "esp_cam_sensor")
    set(ov02c10_priv_reqs driver esp_sccb_intf esp_driver_gpio)
    if(CONFIG_CAMERA_OV02C10)
        set(ov02c10_srcs "ov02c10.c")
        set(ov02c10_priv_inc "private_include")
    endif()
endif()

idf_component_register(
    SRCS ${ov02c10_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${ov02c10_priv_inc}
    REQUIRES ${ov02c10_reqs}
    PRIV_REQUIRES ${ov02c10_priv_reqs}
)

if(IDF_TARGET STREQUAL "esp32p4" AND CONFIG_CAMERA_OV02C10)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ov02c10_detect")
endif()

