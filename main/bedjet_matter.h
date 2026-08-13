#pragma once

#include "bedjet_protocol.h"
#include "bedjet_ble.h"
#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <esp_timer.h>

namespace bedjet {

class BedjetMatter {
public:
    bool init(BedjetBLE* ble);
    void update_status(const BedjetStatusPacket& pkt);
    void sync_clock_now();
    void print_pairing_code();

private:
    BedjetBLE*  ble_ = nullptr;
    esp_matter::node_t*     node_      = nullptr;
    esp_matter::endpoint_t* endpoint_  = nullptr;
    uint16_t                endpoint_id_ = 0;
    esp_timer_handle_t      clock_sync_timer_ = nullptr;

    uint8_t  current_mode_    = MODE_STANDBY;
    float    current_temp_c_   = 0.0f;
    float    target_temp_c_    = 22.0f;
    uint8_t  fan_step_         = 0;

    uint8_t bedjet_mode_to_matter(uint8_t mode);
    uint8_t matter_mode_to_bedjet_button(uint8_t m);

    static esp_err_t thermostat_update_cb(
        esp_matter::callback::type_t type,
        uint16_t endpoint_id, uint32_t cluster_id,
        uint32_t attribute_id, esp_matter_attr_val_t* val);

    static esp_err_t fan_update_cb(
        esp_matter::callback::type_t type,
        uint16_t endpoint_id, uint32_t cluster_id,
        uint32_t attribute_id, esp_matter_attr_val_t* val);

    static void clock_sync_timer_cb(void* arg);
};

} // namespace bedjet
