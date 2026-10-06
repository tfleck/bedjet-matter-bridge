cmake_minimum_required(VERSION 3.16)

# Make sdkconfig.defaults authoritative so a fresh clone (which has no sdkconfig,
# since it is gitignored) still configures correctly.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")

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