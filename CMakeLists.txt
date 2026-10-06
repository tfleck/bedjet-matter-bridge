cmake_minimum_required(VERSION 3.16)

# Transport selection: 'thread' (default) or 'wifi'. Layers sdkconfig.defaults.thread
# or sdkconfig.defaults.wifi on top of the common sdkconfig.defaults. Override with
#   idf.py -DMATTER_TRANSPORT=wifi build
set(MATTER_TRANSPORT "thread" CACHE STRING "Matter transport: thread or wifi")
if(NOT MATTER_TRANSPORT STREQUAL "thread" AND NOT MATTER_TRANSPORT STREQUAL "wifi")
    message(FATAL_ERROR "MATTER_TRANSPORT must be 'thread' or 'wifi' (got '${MATTER_TRANSPORT}')")
endif()

# Make the defaults authoritative so a fresh clone (which has no sdkconfig, since
# it is gitignored) still configures correctly. CONFIG_IDF_TARGET is deliberately
# NOT pinned here so the same tree builds for ESP32-C6 / ESP32-S3 / ESP32-C3 /
# ESP32: run `idf.py set-target <board>` first (CI does).
set(SDKCONFIG_DEFAULTS
    "sdkconfig.defaults"
    "sdkconfig.defaults.${MATTER_TRANSPORT}")
message(STATUS "Matter transport: ${MATTER_TRANSPORT}")

# VendorName/ProductName served by the Basic Information cluster come from the
# CHIP DeviceInstanceInfoProvider, which reads CHIP_DEVICE_CONFIG_DEVICE_*.
# Those macros have #ifndef defaults ("TEST_VENDOR"/"TEST_PRODUCT") and no
# Kconfig knobs. A target_compile_definitions on main would only cover main's
# own objects - the provider and attribute store compile inside the esp-matter
# component - so set them globally before the project() pulls components in.
add_compile_definitions(
    CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME="BedJet Bridge"
    CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME="BedJet V3"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bedjet_matter_bridge)