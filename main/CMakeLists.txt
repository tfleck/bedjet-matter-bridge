idf_component_register(
    SRCS
        "main.cpp"
        "bedjet_ble.cpp"
        "bedjet_matter.cpp"
        "wifi_ota.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        esp-nimble-cpp
        nvs_flash
        esp_matter
        esp_timer
        esp_https_ota
)