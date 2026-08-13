#include "bedjet_matter.h"
#include "esp_log.h"
#include "esp_check.h"
#include "qrcode.h"
#include <time.h>

static const char* TAG = "bedjet_matter";
namespace bedjet {

constexpr uint32_t ATTR_LOCAL_TEMPERATURE          = 0x0000;
constexpr uint32_t ATTR_OCCUPIED_COOLING_SETPOINT  = 0x0011;
constexpr uint32_t ATTR_OCCUPIED_HEATING_SETPOINT  = 0x0012;
constexpr uint32_t ATTR_SYSTEM_MODE                = 0x001C;
constexpr uint32_t ATTR_FAN_PERCENTAGE             = 0x0002;
constexpr uint32_t ATTR_FAN_MODE                   = 0x0000;
constexpr uint32_t ATTR_FAN_STATE                  = 0x0001;

enum MatterSystemMode : uint8_t {
    MATTER_SYS_OFF      = 0,
    MATTER_SYS_AUTO     = 1,
    MATTER_SYS_COOL     = 3,
    MATTER_SYS_HEAT     = 4,
};

enum MatterFanMode : uint8_t {
    FAN_OFF   = 0,
    FAN_ON    = 1,
};

uint8_t BedjetMatter::bedjet_mode_to_matter(uint8_t mode) {
    switch (mode) {
    case MODE_STANDBY: return MATTER_SYS_OFF;
    case MODE_HEAT:
    case MODE_EXTHT:
    case MODE_TURBO:   return MATTER_SYS_HEAT;
    case MODE_COOL:
    case MODE_DRY:     return MATTER_SYS_COOL;
    default:           return MATTER_SYS_OFF;
    }
}

uint8_t BedjetMatter::matter_mode_to_bedjet_button(uint8_t m) {
    switch (m) {
    case MATTER_SYS_OFF:   return BTN_OFF;
    case MATTER_SYS_HEAT:  return BTN_HEAT;
    case MATTER_SYS_COOL:  return BTN_COOL;
    case MATTER_SYS_AUTO:  return BTN_M1;
    default:               return BTN_OFF;
    }
}

bool BedjetMatter::init(BedjetBLE* ble) {
    ble_ = ble;

    node_ = esp_matter::node::create();
    if (!node_) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return false;
    }

    // Thermostat configuration
    esp_matter::cluster::thermostat::config_t thermo_config;
    thermo_config.feature_flags =
        esp_matter::cluster::thermostat::feature::heating::get_id() |
        esp_matter::cluster::thermostat::feature::cooling::get_id();
    thermo_config.local_temperature = 0;
    thermo_config.control_sequence_of_operation = 4;
    thermo_config.system_mode = MATTER_SYS_OFF;
    thermo_config.features.heating.occupied_heating_setpoint = 4400;
    thermo_config.features.cooling.occupied_cooling_setpoint = 3800;

    // Fan configuration
    esp_matter::cluster::fan::config_t fan_config;
    fan_config.features = esp_matter::cluster::fan::feature::percentage_v2::get_id();
    fan_config.speed_percentage = 0;
    fan_config.fan_mode = FAN_OFF;
    fan_config.fan_state = 0;

    // Create endpoint
    esp_matter::endpoint::config_t ep_config = {};
    ep_config.clusters.three_wire = nullptr;
    ep_config.clusters.thermostat = &thermo_config;
    ep_config.clusters.fan = &fan_config;

    endpoint_ = esp_matter::endpoint::create(
        node_,
        esp_matter::device_type::thermostat::id(),
        &ep_config,
        esp_matter::ENDPOINT_FLAG_NONE,
        nullptr);

    if (!endpoint_) {
        ESP_LOGE(TAG, "Failed to create endpoint");
        return false;
    }

    endpoint_id_ = esp_matter::endpoint::get_id(endpoint_);

    // Register callbacks
    esp_matter::callback::register_attribute_callback(thermostat_update_cb);
    esp_matter::callback::register_attribute_callback(fan_update_cb);

    // Start Matter
    esp_err_t err = esp_matter::start(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Matter start failed: %s", esp_err_to_name(err));
        return false;
    }

    // Print QR code after Matter initializes
    vTaskDelay(pdMS_TO_TICKS(2000));
    print_pairing_code();

    // Clock sync timer (every 6 hours)
    esp_timer_create_args_t args = {
        .callback = clock_sync_timer_cb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_sync",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &clock_sync_timer_);
    esp_timer_start_periodic(clock_sync_timer_, 6LL * 3600 * 1000000ULL);

    ESP_LOGI(TAG, "Matter endpoint ready (ID %u)", endpoint_id_);
    return true;
}

void BedjetMatter::update_status(const BedjetStatusPacket& pkt) {
    current_mode_   = pkt.mode;
    current_temp_c_ = temp_step_to_celsius(pkt.actual_temp_step);
    target_temp_c_  = temp_step_to_celsius(pkt.target_temp_step);
    fan_step_       = pkt.fan_step;

    int16_t local_centideg = (int16_t)(current_temp_c_ * 100);
    int16_t target_centideg = (int16_t)(target_temp_c_ * 100);
    uint8_t matter_mode = bedjet_mode_to_matter(pkt.mode);
    uint8_t fan_percent = fan_percent_from_step(fan_step_);
    uint8_t fan_state = (fan_percent > 0 && current_mode_ != MODE_STANDBY) ? 1 : 0;

    ESP_LOGI(TAG, "Update: mode=%d temp=%.1f target=%.1f fan=%d%%",
             pkt.mode, current_temp_c_, target_temp_c_, fan_percent);

    esp_matter_attr_val_t val;

    // Thermostat attributes
    val = esp_matter_attr_val_t{};
    val.type = ESP_MATTER_VAL_TYPE_INT16;
    val.val.i16 = local_centideg;
    esp_matter::attribute::update(endpoint_id_,
        esp_matter::cluster::thermostat::id(),
        ATTR_LOCAL_TEMPERATURE, val);

    val.val.i16 = target_centideg;
    esp_matter::attribute::update(endpoint_id_,
        esp_matter::cluster::thermostat::id(),
        ATTR_OCCUPIED_HEATING_SETPOINT, val);

    val.val.i16 = target_centideg;
    esp_matter::attribute::update(endpoint_id_,
        esp_matter::cluster::thermostat::id(),
        ATTR_OCCUPIED_COOLING_SETPOINT, val);

    val.type = ESP_MATTER_VAL_TYPE_UINT8;
    val.val.u8 = matter_mode;
    esp_matter::attribute::update(endpoint_id_,
        esp_matter::cluster::thermostat::id(),
        ATTR_SYSTEM_MODE, val);

    // Fan attributes
    val.val.u8 = fan_percent;
    esp_matter::attribute::update(endpoint_id_,
        esp_matter::cluster::fan::id(),
        ATTR_FAN_PERCENTAGE, val);

    val.val.u8 = (fan_percent > 0) ? FAN_ON : FAN_OFF;
    esp_matter::attribute::update(endpoint_id_,
        esp_matter::cluster::fan::id(),
        ATTR_FAN_MODE, val);

    val.val.u8 = fan_state;
    esp_matter::attribute::update(endpoint_id_,
        esp_matter::cluster::fan::id(),
        ATTR_FAN_STATE, val);
}

void BedjetMatter::sync_clock_now() {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    uint8_t hrs = timeinfo->tm_hour;
    uint8_t mins = timeinfo->tm_min;

    ESP_LOGI(TAG, "Syncing BedJet clock to %02d:%02d", hrs, mins);
    if (ble_) ble_->sync_clock(hrs, mins);
}

void BedjetMatter::clock_sync_timer_cb(void* arg) {
    auto* self = static_cast<BedjetMatter*>(arg);
    if (self) self->sync_clock_now();
}

void BedjetMatter::print_pairing_code() {
    char* payload = esp_matter::get_setup_payload(node_);

    if (!payload || strlen(payload) == 0) {
        ESP_LOGE(TAG, "Failed to get Matter setup payload");
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "       MATTER PAIRING INFORMATION");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Setup Payload: %s", payload);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Scan this QR code:");
    ESP_LOGI(TAG, "");

    esp_qrcode_config_t qr_config = ESP_QRCODE_CONFIG_DEFAULT();
    esp_qrcode_generate(&qr_config, payload);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Manual pairing code:");

    // Extract 11-digit manual code from payload
    char manual_code[12] = {0};
    strncpy(manual_code, payload + 3, 11);
    ESP_LOGI(TAG, "%s", manual_code);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    free(payload);
}

esp_err_t BedjetMatter::thermostat_update_cb(
    esp_matter::callback::type_t type,
    uint16_t endpoint_id, uint32_t cluster_id,
    uint32_t attribute_id, esp_matter_attr_val_t* val) {

    extern BedjetMatter* g_matter;
    if (type != esp_matter::callback::TYPE_ATTRIBUTE_UPDATE ||
        !g_matter || !g_matter->ble_) return ESP_OK;

    switch (attribute_id) {
    case ATTR_SYSTEM_MODE: {
        uint8_t btn = g_matter->matter_mode_to_bedjet_button(val->val.u8);
        g_matter->ble_->send_button(static_cast<BedjetButton>(btn));
        break;
    }
    case ATTR_OCCUPIED_HEATING_SETPOINT:
    case ATTR_OCCUPIED_COOLING_SETPOINT: {
        float target_c = val->val.i16 / 100.0f;
        g_matter->ble_->set_temperature(target_c);
        break;
    }
    default:
        break;
    }

    return ESP_OK;
}

esp_err_t BedjetMatter::fan_update_cb(
    esp_matter::callback::type_t type,
    uint16_t endpoint_id, uint32_t cluster_id,
    uint32_t attribute_id, esp_matter_attr_val_t* val) {

    extern BedjetMatter* g_matter;
    if (type != esp_matter::callback::TYPE_ATTRIBUTE_UPDATE ||
        !g_matter || !g_matter->ble_) return ESP_OK;

    switch (attribute_id) {
    case ATTR_FAN_PERCENTAGE: {
        uint8_t fan_step = fan_step_from_percent(val->val.u8);
        g_matter->ble_->set_fan_step(fan_step);
        break;
    }
    default:
        break;
    }

    return ESP_OK;
}

} // namespace bedjet
