#include "bedjet_ble.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include <cstring>
#include <algorithm>

static const char* TAG = "bedjet_ble";
namespace bedjet {

BedjetBLE* BedjetBLE::instance_ = nullptr;

static void bedjet_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    if (BedjetBLE::instance_)
        BedjetBLE::instance_->handle_gap_event(event, param);
}

static void bedjet_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
    if (BedjetBLE::instance_)
        BedjetBLE::instance_->handle_gattc_event(event, gattc_if, param);
}

bool BedjetBLE::init() {
    instance_ = this;

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_err_t ret = esp_ble_gattc_register_callback(bedjet_gattc_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTC callback failed");
        return false;
    }

    uint8_t app_id = 0;
    ret = esp_ble_gattc_app_register(app_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTC app register failed");
        return false;
    }

    ret = esp_ble_gap_register_callback(bedjet_gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP callback failed");
        return false;
    }

    ESP_LOGI(TAG, "BLE client initialized");
    return true;
}

void BedjetBLE::connect(const uint8_t mac[6]) {
    memcpy(target_mac_, mac, 6);

    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };

    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_params failed");
        return;
    }

    ret = esp_ble_gap_start_scanning(30);
    if (ret == ESP_OK) {
        scanning_ = true;
        ESP_LOGI(TAG, "Scanning for BedJet...");
    }
}

void BedjetBLE::disconnect() {
    if (connected_) {
        esp_ble_gattc_close(gattc_if_, conn_id_);
    }
    if (scanning_) {
        esp_ble_gap_stop_scanning();
        scanning_ = false;
    }
}

bool BedjetBLE::send_packet(const BedjetPacket& pkt) {
    if (!connected_ || cmd_handle_ == 0) return false;

    uint8_t buf[6];
    buf[0] = pkt.command;
    memcpy(buf + 1, pkt.data, pkt.data_len);
    uint16_t total = 1 + pkt.data_len;

    esp_err_t ret = esp_ble_gattc_write_char(
        gattc_if_, conn_id_, cmd_handle_,
        total, buf,
        ESP_GATT_WRITE_TYPE_RSP,
        ESP_GATT_AUTH_REQ_NONE);

    return ret == ESP_OK;
}

bool BedjetBLE::send_button(BedjetButton btn) {
    return send_packet(BedjetPacket::button(btn));
}

bool BedjetBLE::set_temperature(float celsius) {
    celsius = std::clamp(celsius, 19.0f, 40.0f);
    return send_packet(BedjetPacket::set_temp(celsius));
}

bool BedjetBLE::set_fan_step(uint8_t step) {
    step = std::min(step, (uint8_t)19);
    return send_packet(BedjetPacket::set_fan(step));
}

bool BedjetBLE::request_status() {
    return send_packet(BedjetPacket::request_status());
}

bool BedjetBLE::sync_clock(uint8_t hrs, uint8_t mins) {
    return send_packet(BedjetPacket::set_clock(hrs, mins));
}

void BedjetBLE::handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        auto* res = &param->scan_rst;
        if (res->search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            if (memcmp(res->bda, target_mac_, 6) == 0) {
                ESP_LOGI(TAG, "BedJet found!");
                esp_ble_gap_stop_scanning();
                scanning_ = false;
                open_connection();
            }
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan stopped");
        break;

    default:
        break;
    }
}

void BedjetBLE::handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
    gattc_if_ = gattc_if;

    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "GATTC app registered");
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Open failed");
            vTaskDelay(pdMS_TO_TICKS(5000));
            connect(target_mac_);
        } else {
            conn_id_ = param->open.conn_id;
            ESP_LOGI(TAG, "Connected");
            esp_ble_gattc_search_service(gattc_if, conn_id_, nullptr);
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGI(TAG, "Service discovery complete");
        discover_characteristics();
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Notifications enabled!");
            connected_ = true;
            if (conn_cb_) conn_cb_(true);
            request_status();
        }
        break;

    case ESP_GATTC_NOTIFY_EVT:
        assemble_status(param->notify.value, param->notify.value_len);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "Disconnected");
        connected_ = false;
        if (conn_cb_) conn_cb_(false);
        vTaskDelay(pdMS_TO_TICKS(5000));
        connect(target_mac_);
        break;

    default:
        break;
    }
}

void BedjetBLE::open_connection() {
    esp_ble_gattc_open(gattc_if_, target_mac_, BLE_ADDR_TYPE_PUBLIC, true);
}

void BedjetBLE::discover_characteristics() {
    esp_ble_gattc_get_service(gattc_if_, conn_id_, nullptr);
    // In production: iterate services, match BedJet UUIDs,
    // store cmd_handle_ and status_handle_, then call enable_notifications()
}

void BedjetBLE::enable_notifications() {
    if (status_handle_ == 0) return;

    uint8_t enable[] = {0x01, 0x00};
    esp_ble_gattc_write_char_descr(
        gattc_if_, conn_id_,
        status_handle_ + 1,
        sizeof(enable), enable,
        ESP_GATT_WRITE_TYPE_RSP,
        ESP_GATT_AUTH_REQ_NONE);
}

void BedjetBLE::assemble_status(const uint8_t* data, uint16_t len) {
    if (len == 0 || len > sizeof(status_buf_)) return;

    BedjetStatusPacket* hdr = (BedjetStatusPacket*)data;

    if (hdr->is_partial == 1) {
        memcpy(status_buf_, data, len);
        status_len_ = len;
        esp_ble_gattc_read_char(gattc_if_, conn_id_, status_handle_, ESP_GATT_AUTH_REQ_NONE);
        return;
    }

    if (status_len_ > 0) {
        uint16_t remaining = sizeof(status_buf_) - status_len_;
        uint16_t to_copy = std::min(len, remaining);
        memcpy(status_buf_ + status_len_, data, to_copy);
        status_len_ += to_copy;

        if (status_len_ >= sizeof(BedjetStatusPacket)) {
            auto* pkt = (BedjetStatusPacket*)status_buf_;
            if (status_cb_) status_cb_(*pkt);
        }
        status_len_ = 0;
    } else {
        if (len >= sizeof(BedjetStatusPacket)) {
            auto* pkt = (BedjetStatusPacket*)data;
            if (status_cb_) status_cb_(*pkt);
        }
    }
}

} // namespace bedjet
