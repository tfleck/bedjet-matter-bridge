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
#
# CHIP_DEVICE_CONFIG_DEVICE_NAME is the name carried in the commissionable
# DNS-SD "DN" TXT record (Server/Dnssd.cpp), which Apple Home uses to pre-fill
# the accessory name in the Add Accessory flow. It is only advertised when
# CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME is 1 (default 0, and the
# ESP32 platform config has no Kconfig for either), otherwise Home shows its
# generic "Matter Accessory" placeholder. NodeLabel is handled separately (see
# apply_identity) and only applies after commissioning.
add_compile_definitions(
    CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME="BedJet Bridge"
    CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME="BedJet V3"
    CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME=1
    CHIP_DEVICE_CONFIG_DEVICE_NAME="BedJet"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bedjet_matter_bridge)