#pragma once

#include <stdint.h>
#include <functional>
#include "bedjet_protocol.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"

namespace bedjet {

using StatusCallback = std::function<void(const BedjetStatusPacket&)>;
using ConnStateCallback = std::function<void(bool connected)>;

class BedjetBLE {
public:
    bool init();
    void connect(const uint8_t mac[6]);
    void disconnect();
    bool send_packet(const BedjetPacket& pkt);

    bool send_button(BedjetButton btn);
    bool set_temperature(float celsius);
    bool set_fan_step(uint8_t step);
    bool request_status();
    bool sync_clock(uint8_t hrs, uint8_t mins);

    bool is_connected() const { return connected_; }

    void on_status(StatusCallback cb)         { status_cb_ = std::move(cb); }
    void on_conn_state(ConnStateCallback cb)   { conn_cb_ = std::move(cb); }

private:
    void handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
    void handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param);

    void open_connection();
    void discover_characteristics();
    void enable_notifications();
    void assemble_status(const uint8_t* data, uint16_t len);

    friend void bedjet_gap_cb(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t*);
    friend void bedjet_gattc_cb(esp_gattc_cb_event_t, esp_gatt_if_t, esp_ble_gattc_cb_param_t*);

    bool          connected_     = false;
    bool          scanning_      = false;
    uint16_t      conn_id_       = 0;
    esp_gatt_if_t gattc_if_      = 0;
    uint8_t       target_mac_[6]{};
    uint16_t      cmd_handle_    = 0;
    uint16_t      status_handle_  = 0;

    uint8_t       status_buf_[128]{};
    uint16_t      status_len_    = 0;

    StatusCallback    status_cb_;
    ConnStateCallback conn_cb_;

    static BedjetBLE* instance_;
};

} // namespace bedjet
