#include "bedjet_matter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <esp_mac.h>

#include <setup_payload/OnboardingCodesUtil.h>

#include "esp_log.h"
#include "esp_matter_mem.h"
#include "esp_system.h"

namespace bedjet {

static const char *TAG = "bedjet_matter";

namespace {

constexpr uint32_t CLUSTER_THERMOSTAT =
    chip::app::Clusters::Thermostat::Id;
constexpr uint32_t CLUSTER_FAN_CONTROL =
    chip::app::Clusters::FanControl::Id;

constexpr uint32_t ATTR_LOCAL_TEMPERATURE =
    chip::app::Clusters::Thermostat::Attributes::LocalTemperature::Id;
constexpr uint32_t ATTR_OCCUPIED_HEATING_SETPOINT =
    chip::app::Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::Id;
constexpr uint32_t ATTR_OCCUPIED_COOLING_SETPOINT =
    chip::app::Clusters::Thermostat::Attributes::OccupiedCoolingSetpoint::Id;
constexpr uint32_t ATTR_SYSTEM_MODE =
    chip::app::Clusters::Thermostat::Attributes::SystemMode::Id;

constexpr uint32_t ATTR_FAN_MODE =
    chip::app::Clusters::FanControl::Attributes::FanMode::Id;
constexpr uint32_t ATTR_PERCENT_SETTING =
    chip::app::Clusters::FanControl::Attributes::PercentSetting::Id;
constexpr uint32_t ATTR_PERCENT_CURRENT =
    chip::app::Clusters::FanControl::Attributes::PercentCurrent::Id;

// chip::app::Clusters::Thermostat::SystemModeEnum
enum MatterSystemMode : uint8_t {
    MATTER_SYS_OFF      = 0x00,
    MATTER_SYS_AUTO     = 0x01,
    MATTER_SYS_COOL     = 0x03,
    MATTER_SYS_HEAT     = 0x04,
    MATTER_SYS_FAN_ONLY = 0x07,
    MATTER_SYS_DRY      = 0x08,
};

// chip::app::Clusters::FanControl::FanModeEnum. The cluster advertises
// FanModeSequence kOffLowMedHigh, so only these four values are ever published.
enum MatterFanMode : uint8_t {
    FAN_MODE_OFF    = 0x00,
    FAN_MODE_LOW    = 0x01,
    FAN_MODE_MEDIUM = 0x02,
    FAN_MODE_HIGH   = 0x03,
};

constexpr uint8_t CMD_QUEUE_LEN  = 8;
constexpr uint32_t BRIDGE_STACK  = 6144;

// sets a flag for the lifetime of the scope.
//
// esp_matter::attribute::report() on a *writable* attribute is routed through the
// CHIP data model provider's WriteAttribute path, and that path fires the
// esp-matter PRE_UPDATE callback (esp_matter_data_model_provider.cpp) before the
// cluster accepts the value. So a device-originated report of SystemMode,
// OccupiedHeating/CoolingSetpoint, PercentSetting or FanMode would otherwise be
// seen as a controller write and turned back into a BedJet command.
//
// The guard is held for exactly one report() call (constructed inside each
// publish_*()), not for the whole apply_status(). A report() takes the CHIP
// stack lock and runs its PRE_UPDATE callback synchronously on this task, so
// the flag only needs to cover that call; holding it across several reports
// would silently drop a controller write that arrived in between.
class PublishGuard {
public:
    explicit PublishGuard(std::atomic<bool>& flag) : flag_(flag)
    {
        flag_.store(true, std::memory_order_release);
    }
    ~PublishGuard() { flag_.store(false, std::memory_order_release); }

    PublishGuard(const PublishGuard&)            = delete;
    PublishGuard& operator=(const PublishGuard&) = delete;

private:
    std::atomic<bool>& flag_;
};

} // namespace

BedjetMatter* g_matter = nullptr;

// ---------------------------------------------------------------------------
// Mapping helpers
// ---------------------------------------------------------------------------

uint8_t BedjetMatter::bedjet_mode_to_system_mode(uint8_t mode)
{
    switch (mode) {
    case MODE_STANDBY: return MATTER_SYS_OFF;
    case MODE_HEAT:
    case MODE_TURBO:
    case MODE_EXTHT:   return MATTER_SYS_HEAT;
    case MODE_COOL:    return MATTER_SYS_COOL;
    case MODE_DRY:     return MATTER_SYS_DRY;
    case MODE_WAIT:    return MATTER_SYS_OFF;
    default:           return MATTER_SYS_OFF;
    }
}

int BedjetMatter::system_mode_to_button(uint8_t matter_system_mode)
{
    switch (matter_system_mode) {
    case MATTER_SYS_OFF:      return BTN_OFF;
    case MATTER_SYS_HEAT:     return BTN_HEAT;
    case MATTER_SYS_COOL:
    case MATTER_SYS_FAN_ONLY: return BTN_COOL;
    case MATTER_SYS_DRY:      return BTN_DRY;
    // The BedJet has no automatic mode or setpoint dead band, so Auto cannot be
    // honoured. Refuse it rather than silently sending a different command.
    default:                  return -1;
    }
}

// The BedJet reports 5%..100% in 20 discrete steps. Matter FanMode is a coarse
// band, so each band is anchored to a step the BedJet can actually produce.
uint8_t BedjetMatter::percent_to_fan_mode(uint8_t percent)
{
    if (percent == 0)                     return FAN_MODE_OFF;
    if (percent <= BEDJET_FAN_PERCENT_MIN) return FAN_MODE_LOW;
    if (percent <= 66)                     return FAN_MODE_MEDIUM;
    return FAN_MODE_HIGH;
}

uint8_t BedjetMatter::fan_mode_to_percent(uint8_t matter_fan_mode)
{
    switch (matter_fan_mode) {
    case FAN_MODE_LOW:    return BEDJET_FAN_PERCENT_MIN;                 // step 0  (5%)
    case FAN_MODE_MEDIUM: return fan_percent_from_step(6);               // step 6  (35%)
    case FAN_MODE_HIGH:   return fan_percent_from_step(BEDJET_FAN_STEP_MAX); // step 19 (100%)
    case FAN_MODE_OFF:
    default:              return 0;
    }
}

float BedjetMatter::clamp_temp(float celsius) const
{
    if (celsius < temp_min_c_) return temp_min_c_;
    if (celsius > temp_max_c_) return temp_max_c_;
    return celsius;
}

int16_t BedjetMatter::centi_from_temp(float celsius) const
{
    // Snap FIRST to the BedJet's half-degree wire resolution, then scale with
    // integer math: a Matter write of e.g. 22.2 C must become a whole step
    // (44 = 22.0 C -> 2200) rather than a float-rounded 2220 that the BedJet
    // can never actually hold.
    //
    // Write-path rounding uses round-half-up to the nearest step so an integer
    // Fahrenheit setpoint from HomeKit converts back to the same Fahrenheit
    // reading: HomeKit sends centi-C rounded from F, we snap to the nearest
    // half-degree, and step*50 converts back to the same F (verified for every
    // integer F whose nearest step lies in 19-43 C). Truncation instead would
    // drop up to half a degree and read back 1 F low on a third of the range.
    const uint8_t step = celsius_to_temp_step(clamp_temp(celsius));
    return static_cast<int16_t>(static_cast<int32_t>(step) * 50);
}

// Centi-degrees for one wire step, using integer math so the value is exact.
// A step is exactly half a degree, so step N is exactly N*50 centi-degrees;
// float scaling (step / 2.0f * 100) can wobble by a hundredth and tip
// HomeKit's Fahrenheit rounding a degree off the radio remote.
int16_t BedjetMatter::centi_from_step(uint8_t step)
{
    return static_cast<int16_t>(static_cast<int32_t>(step) * 50);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool BedjetMatter::init(BedjetBLE *ble)
{
    g_matter = this;
    ble_     = ble;

    esp_matter::node::config_t node_config;
    // Root endpoint Basic Information cluster: NodeLabel is the user-visible
    // accessory name in Home. The node_config label is only the create-time
    // default: NodeLabel is WRITABLE + NONVOLATILE, so the CHIP BasicInformation
    // cluster (PolicyBased.h) loads the persisted NVS copy on Startup and
    // serves that instead. On a commissioned node that already stored the old
    // value ("Matter Accessory" / empty), the config default never surfaces -
    // mark_matter_started() below overwrites it via the provider WriteAttribute
    // path so the rename actually lands.
    strncpy(node_config.root_node.basic_information.node_label, "BedJet",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    // VendorName/ProductName are not fields of basic_information::config_t
    // (only node_label/unique_id are). They are supplied globally through
    // CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME / _PRODUCT_NAME in CMakeLists.txt.
    node_ = esp_matter::node::create(&node_config, matter_attribute_update_cb,
                                     matter_identification_cb, this);
    if (!node_) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return false;
    }

    // BasicInformation.SerialNumber is only created when
    // CONFIG_ESP_MATTER_ENABLE_OPTIONAL_ATTRIBUTES is set (it is not here), so
    // controllers that read it get an unsupported-attribute error. Create it
    // from the chip's base MAC so it is stable and unique per unit.
    {
        uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);
        char serial[13];
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        esp_matter::endpoint_t *ep0 = esp_matter::endpoint::get(0);
        esp_matter::cluster_t *bi =
            ep0 ? esp_matter::cluster::get(ep0, chip::app::Clusters::BasicInformation::Id)
                : nullptr;
        if (!bi || !esp_matter::cluster::basic_information::attribute::create_serial_number(
                       bi, serial, sizeof(serial))) {
            ESP_LOGW(TAG, "Could not set BasicInformation SerialNumber");
        }
    }

    // ---- Endpoint 1: Thermostat ----
    esp_matter::cluster::thermostat::config_t thermo_config;
    thermo_config.feature_flags =
        chip::to_underlying(chip::app::Clusters::Thermostat::Feature::kHeating) |
        chip::to_underlying(chip::app::Clusters::Thermostat::Feature::kCooling);
    thermo_config.local_temperature = static_cast<int16_t>(0);
    // kCoolingAndHeating: both features are declared above.
    thermo_config.control_sequence_of_operation = 0x04;
    thermo_config.system_mode                  = MATTER_SYS_OFF;
    thermo_config.features.heating.occupied_heating_setpoint = static_cast<int16_t>(3000);
    thermo_config.features.cooling.occupied_cooling_setpoint = static_cast<int16_t>(2400);

    esp_matter::endpoint::thermostat::config_t thermo_ep_config;
    thermo_ep_config.thermostat = thermo_config;

    thermo_ep_ = esp_matter::endpoint::thermostat::create(
        node_, &thermo_ep_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (!thermo_ep_) {
        ESP_LOGE(TAG, "Failed to create thermostat endpoint");
        return false;
    }
    thermo_ep_id_ = esp_matter::endpoint::get_id(thermo_ep_);

    // ---- Endpoint 2: Fan ----
    // Fan Control is not valid on a Thermostat endpoint (device type 0x0301), so
    // the fan lives on its own endpoint (device type 0x002B). Without the
    // MultiSpeed feature the cluster reports speed via PercentSetting /
    // PercentCurrent in whole percent, which is what the BedJet's 20 steps map
    // onto.
    esp_matter::cluster::fan_control::config_t fan_cluster_config;
    fan_cluster_config.fan_mode          = FAN_MODE_OFF;
    fan_cluster_config.fan_mode_sequence = 0;   // kOffLowMedHigh
    fan_cluster_config.percent_setting   = static_cast<uint8_t>(0);
    fan_cluster_config.percent_current   = 0;

    esp_matter::endpoint::fan::config_t fan_ep_config;
    fan_ep_config.fan_control = fan_cluster_config;

    fan_ep_ = esp_matter::endpoint::fan::create(
        node_, &fan_ep_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (!fan_ep_) {
        ESP_LOGE(TAG, "Failed to create fan endpoint");
        return false;
    }
    fan_ep_id_ = esp_matter::endpoint::get_id(fan_ep_);

    // status_queue_ is length 1 and written with xQueueOverwrite so the bridge
    // task always sees the newest state rather than a backlog of stale frames.
    status_queue_ = xQueueCreate(1, sizeof(BedjetNotification));
    cmd_queue_    = xQueueCreate(CMD_QUEUE_LEN, sizeof(MatterCommand));
    if (!status_queue_ || !cmd_queue_) {
        ESP_LOGE(TAG, "Failed to allocate Matter bridge queues");
        return false;
    }
    // A queue set lets the bridge task block indefinitely until either queue
    // has data, instead of waking every 500 ms on a status timeout. Both
    // queues are members so xQueueSelectFromSet identifies the source.
    queue_set_ = xQueueCreateSet(CMD_QUEUE_LEN + 1);
    if (!queue_set_) {
        ESP_LOGE(TAG, "Failed to allocate Matter bridge queue set");
        return false;
    }
    if (xQueueAddToSet(cmd_queue_, queue_set_) != pdPASS ||
        xQueueAddToSet(status_queue_, queue_set_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to add queues to Matter bridge queue set");
        return false;
    }

    if (xTaskCreate(&BedjetMatter::bridge_task_entry, "bedjet_bridge",
                    BRIDGE_STACK, this, 4, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn Matter bridge task");
        return false;
    }

    ESP_LOGI(TAG, "Matter endpoints created: thermostat=%u fan=%u",
             thermo_ep_id_, fan_ep_id_);
    return true;
}

// ---------------------------------------------------------------------------
// Bridge task
// ---------------------------------------------------------------------------

void BedjetMatter::bridge_task_entry(void *arg)
{
    static_cast<BedjetMatter *>(arg)->run_bridge();
}

void BedjetMatter::mark_matter_started()
{
    apply_identity();
}

void BedjetMatter::apply_identity()
{
    // Overwrite the served NodeLabel via the unified set_val path, which
    // routes WRITABLE attributes through the CHIP WriteAttribute machinery:
    // that updates the provider's mNodeLabel AND persists it to NVS, so the
    // rename survives reboots and is what Home reads back. Only writes when
    // the value differs, so a user rename in Home is never clobbered.
    static constexpr uint32_t kBasicInfo =
        chip::app::Clusters::BasicInformation::Id;
    static constexpr uint32_t kNodeLabel =
        chip::app::Clusters::BasicInformation::Attributes::NodeLabel::Id;

    esp_matter_attr_val_t current{};
    {
        // get_val() reads the data model provider directly and, unlike
        // report()/set_val(), takes no CHIP stack lock of its own. apply_identity
        // runs on the app_main task after esp_matter::start(), so lock the read.
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        if (esp_matter::attribute::get_val(0, kBasicInfo, kNodeLabel, &current) != ESP_OK) {
            return;
        }
    }

    // get_val() hands back a heap copy for string attributes (ownership passes
    // to the caller), so free it once compared instead of leaking per boot.
    const bool is_string =
        (current.type == ESP_MATTER_VAL_TYPE_CHAR_STRING ||
         current.type == ESP_MATTER_VAL_TYPE_LONG_CHAR_STRING);
    const char *cur_str =
        (is_string && current.val.a.b != nullptr) ? (const char *)current.val.a.b : "";
    const bool already_named = (std::strncmp(cur_str, "BedJet", sizeof("BedJet")) == 0);
    if (is_string && current.val.a.b != nullptr) {
        esp_matter_mem_free(current.val.a.b);
        current.val.a.b = nullptr;
    }
    if (already_named) {
        return;
    }

    char label[] = "BedJet";
    esp_matter_attr_val_t val = esp_matter_char_str(label, sizeof(label) - 1);
    const esp_err_t err =
        esp_matter::attribute::set_val(0, kBasicInfo, kNodeLabel, &val, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set NodeLabel: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Accessory name set to 'BedJet'");
    }
}

void BedjetMatter::post_status(const BedjetNotification &n)
{
    if (status_queue_) {
        xQueueOverwrite(status_queue_, &n);
    }
}

void BedjetMatter::run_bridge()
{
    for (;;) {
        // Block until either queue has data. One item from the signalled queue
        // is handled, then at most one extra from each queue, so a burst on one
        // side cannot starve the other while the task still drains promptly.
        const QueueSetMemberHandle_t ready =
            xQueueSelectFromSet(queue_set_, portMAX_DELAY);
        if (ready == nullptr) {
            continue;
        }
        if (ready == cmd_queue_) {
            MatterCommand cmd{};
            if (xQueueReceive(cmd_queue_, &cmd, 0) == pdTRUE) {
                execute_command(cmd);
            }
        } else if (ready == status_queue_) {
            BedjetNotification n{};
            if (xQueueReceive(status_queue_, &n, 0) == pdTRUE) {
                apply_status(n);
            }
        }
        // Drain at most one extra item from the *other* queue so bursts on
        // both sides make progress without one side monopolising the task.
        MatterCommand cmd{};
        if (xQueueReceive(cmd_queue_, &cmd, 0) == pdTRUE) {
            execute_command(cmd);
        }
        BedjetNotification n{};
        if (xQueueReceive(status_queue_, &n, 0) == pdTRUE) {
            apply_status(n);
        }
    }
}

void BedjetMatter::execute_command(const MatterCommand &cmd)
{
    switch (cmd.kind) {
    case MATTER_CMD_SYSTEM_MODE:     do_system_mode(static_cast<uint8_t>(cmd.value));      break;
    case MATTER_CMD_SETPOINT:        do_setpoint(static_cast<int16_t>(cmd.value));         break;
    case MATTER_CMD_PERCENT_SETTING: do_percent_setting(static_cast<uint8_t>(cmd.value));  break;
    case MATTER_CMD_FAN_MODE:        do_fan_mode(static_cast<uint8_t>(cmd.value));        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// BedJet -> Matter
// ---------------------------------------------------------------------------

void BedjetMatter::learn_temp_limits(const BedjetNotification &n)
{
    if (n.min_temp_step == 0 || n.max_temp_step <= n.min_temp_step) {
        return;
    }
    const float lo = temp_step_to_celsius(n.min_temp_step);
    const float hi = temp_step_to_celsius(n.max_temp_step);
    // Ignore implausible values rather than clamping the device into a corner.
    if (lo >= BEDJET_TEMP_MIN_C - 1.0f && hi <= BEDJET_TEMP_MAX_C + 1.0f) {
        temp_min_c_ = lo;
        temp_max_c_ = hi;
    }
}

void BedjetMatter::apply_status(const BedjetNotification &n)
{
    learn_temp_limits(n);

    device_mode_ = n.mode;
    fan_step_    = n.fan_step;

    const bool   running = (n.mode != MODE_STANDBY) && (n.mode != MODE_WAIT);
    const uint8_t current_percent = running ? fan_percent_from_step(n.fan_step) : 0;

    if (!fan_target_valid_) {
        // Until a controller writes a speed, track what the device is doing.
        fan_target_percent_ = current_percent;
        fan_target_valid_   = true;
    }

    const uint8_t system_mode = bedjet_mode_to_system_mode(n.mode);
    // LocalTemperature is the ROOM temperature, which the BedJet measures with
    // its ambient sensor (byte 17). The outlet sensor (byte 7, actual_temp_step)
    // reads the discharge air, which runs hotter/colder than the room while the
    // BedJet is driving the bed - publishing it as the room temperature is what
    // put HomeKit 1 F (and often much more) off the radio remote. This matches
    // the ESPHome reference, whose climate current-temperature defaults to the
    // ambient sensor with the outlet available only as an opt-in sensor.
    //
    // Both sensors share the half-degree wire resolution (step = C * 2), so each
    // is published with exact integer math: step * 50 centi-degrees is always a
    // whole multiple of 50 and never carries float wobble into HomeKit's
    // Fahrenheit rounding.
    const int16_t local_centi  = centi_from_step(n.ambient_temp_step);
    const int16_t target_centi = centi_from_step(n.target_temp_step);
    const uint8_t fan_mode     = percent_to_fan_mode(current_percent);

    if (n.mode != last_logged_mode_ || n.fan_step != last_logged_step_ ||
        target_centi != last_logged_target_) {
        ESP_LOGI(TAG, "BedJet mode=%u room=%.1fC outlet=%.1fC target=%.1fC (device range %.1f-%.1fC) fan_step=%u (%u%%)",
                 n.mode, temp_step_to_celsius(n.ambient_temp_step),
                 temp_step_to_celsius(n.actual_temp_step),
                 temp_step_to_celsius(n.target_temp_step),
                 temp_min_c_, temp_max_c_, n.fan_step, current_percent);
        last_logged_mode_   = n.mode;
        last_logged_step_   = n.fan_step;
        last_logged_target_ = target_centi;
    }

    // Each publish_*() raises publishing_ for the duration of its own report()
    // only, so a device-originated write is not mistaken for a controller
    // request (see PublishGuard) while a controller write landing between two
    // publishes is still honoured.
    //
    // Baselines only advance on success: a failed report is retried on the
    // next status instead of being lost until the value changes again.
    if (local_centi != last_local_centi_) {
        if (publish_nullable_i16(thermo_ep_id_, CLUSTER_THERMOSTAT, ATTR_LOCAL_TEMPERATURE, local_centi)) {
            last_local_centi_ = local_centi;
        }
    }

    // The BedJet has a single target temperature. Publish it into whichever
    // setpoint matches the current system mode instead of writing both, which
    // would advertise an impossible "heat to N and cool to N" range.
    if (system_mode == MATTER_SYS_HEAT) {
        if (target_centi != last_heat_centi_) {
            if (publish_i16(thermo_ep_id_, CLUSTER_THERMOSTAT, ATTR_OCCUPIED_HEATING_SETPOINT, target_centi)) {
                last_heat_centi_ = target_centi;
            }
        }
    } else if (system_mode == MATTER_SYS_COOL || system_mode == MATTER_SYS_DRY) {
        if (target_centi != last_cool_centi_) {
            if (publish_i16(thermo_ep_id_, CLUSTER_THERMOSTAT, ATTR_OCCUPIED_COOLING_SETPOINT, target_centi)) {
                last_cool_centi_ = target_centi;
            }
        }
    }

    if (system_mode != last_system_mode_) {
        if (publish_enum8(thermo_ep_id_, CLUSTER_THERMOSTAT, ATTR_SYSTEM_MODE, system_mode)) {
            last_system_mode_ = system_mode;
        }
    }

    // Fan: PercentCurrent is the measured speed, PercentSetting is the commanded
    // target, FanMode is the coarse band derived from the measured speed.
    if (current_percent != last_percent_current_) {
        if (publish_u8(fan_ep_id_, CLUSTER_FAN_CONTROL, ATTR_PERCENT_CURRENT, current_percent)) {
            last_percent_current_ = current_percent;
        }
    }
    if (fan_target_percent_ != last_percent_setting_) {
        if (publish_nullable_u8(fan_ep_id_, CLUSTER_FAN_CONTROL, ATTR_PERCENT_SETTING, fan_target_percent_)) {
            last_percent_setting_ = fan_target_percent_;
        }
    }
    if (fan_mode != last_fan_mode_) {
        if (publish_enum8(fan_ep_id_, CLUSTER_FAN_CONTROL, ATTR_FAN_MODE, fan_mode)) {
            last_fan_mode_ = fan_mode;
        }
    }
}

bool BedjetMatter::publish_enum8(uint16_t endpoint_id, uint32_t cluster_id,
                                 uint32_t attribute_id, uint8_t value)
{
    // SystemMode and FanMode are Matter enums (ENUM8), not plain UINT8.
    // esp-matter stores them as ESP_MATTER_VAL_TYPE_ENUM8, so reporting with
    // UINT8 fails get_val_type() with ESP_ERR_INVALID_ARG (err 258).
    esp_matter_attr_val_t val = esp_matter_enum8(value);

    const PublishGuard guard(publishing_);
    const esp_err_t err = esp_matter::attribute::report(endpoint_id, cluster_id, attribute_id, &val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "report ep=%u cl=%" PRIX32 " at=%" PRIX32 " = %u -> %s",
                 endpoint_id, cluster_id, attribute_id, value, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool BedjetMatter::publish_u8(uint16_t endpoint_id, uint32_t cluster_id,
                              uint32_t attribute_id, uint8_t value)
{
    esp_matter_attr_val_t val = esp_matter_uint8(value);

    const PublishGuard guard(publishing_);
    const esp_err_t err = esp_matter::attribute::report(endpoint_id, cluster_id, attribute_id, &val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "report ep=%u cl=%" PRIX32 " at=%" PRIX32 " = %u -> %s",
                 endpoint_id, cluster_id, attribute_id, value, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool BedjetMatter::publish_nullable_u8(uint16_t endpoint_id, uint32_t cluster_id,
                                       uint32_t attribute_id, uint8_t value)
{
    // PercentSetting is a nullable UINT8. The cluster config initialised it
    // from chip::NullOptional, so its stored type is
    // ESP_MATTER_VAL_TYPE_NULLABLE_UINT8; reporting a plain UINT8 fails the
    // get_val_type() check with ESP_ERR_INVALID_ARG (err 258).
    chip::app::DataModel::Nullable<uint8_t> v;
    v.SetNonNull(value);
    esp_matter_attr_val_t val = esp_matter_nullable_uint8(nullable<uint8_t>(v.Value()));

    const PublishGuard guard(publishing_);
    const esp_err_t err = esp_matter::attribute::report(endpoint_id, cluster_id, attribute_id, &val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "report ep=%u cl=%" PRIX32 " at=%" PRIX32 " = %u -> %s",
                 endpoint_id, cluster_id, attribute_id, value, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool BedjetMatter::publish_nullable_i16(uint16_t endpoint_id, uint32_t cluster_id,
                                        uint32_t attribute_id, int16_t value)
{
    // LocalTemperature is a nullable INT16 for the same reason as above.
    chip::app::DataModel::Nullable<int16_t> v;
    v.SetNonNull(value);
    esp_matter_attr_val_t val = esp_matter_nullable_int16(nullable<int16_t>(v.Value()));

    const PublishGuard guard(publishing_);
    const esp_err_t err = esp_matter::attribute::report(endpoint_id, cluster_id, attribute_id, &val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "report ep=%u cl=%" PRIX32 " at=%" PRIX32 " = %d -> %s",
                 endpoint_id, cluster_id, attribute_id, value, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool BedjetMatter::publish_i16(uint16_t endpoint_id, uint32_t cluster_id,
                               uint32_t attribute_id, int16_t value)
{
    esp_matter_attr_val_t val = esp_matter_int16(value);

    const PublishGuard guard(publishing_);
    const esp_err_t err = esp_matter::attribute::report(endpoint_id, cluster_id, attribute_id, &val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "report ep=%u cl=%" PRIX32 " at=%" PRIX32 " = %d -> %s",
                 endpoint_id, cluster_id, attribute_id, value, esp_err_to_name(err));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Matter -> BedJet
// ---------------------------------------------------------------------------

bool BedjetMatter::do_system_mode(uint8_t matter_system_mode)
{
    const int btn = system_mode_to_button(matter_system_mode);
    if (btn < 0) {
        ESP_LOGW(TAG, "SystemMode %u has no BedJet equivalent, ignoring",
                 matter_system_mode);
        return false;
    }
    if (!ble_) {
        return false;
    }
    ESP_LOGI(TAG, "SystemMode %u -> button 0x%02X", matter_system_mode, btn);
    return ble_->send_button(static_cast<BedjetButton>(btn));
}

bool BedjetMatter::do_setpoint(int16_t centi_degrees)
{
    if (!ble_) {
        return false;
    }
    // Snap the Matter centi-degrees to the nearest half-degree wire step with
    // round-half-up (celsius_to_temp_step uses lroundf). HomeKit sends integer
    // Fahrenheit as centi-C, and nearest-step rounding maps every such value
    // back to a step whose published centi-degrees (step * 50) displays as the
    // same Fahrenheit figure - verified across the 19-43 C wire range.
    // Truncation instead would read back 1 F low on steps whose exact
    // Fahrenheit fraction is >= .5 (e.g. 22.0 C = 71.6 F showing 71).
    const float celsius = static_cast<float>(centi_degrees) / 100.0f;
    const float clamped = clamp_temp(celsius);
    if (clamped != celsius) {
        ESP_LOGW(TAG, "Setpoint %.2fC clamped to device range %.1f-%.1fC",
                 celsius, temp_min_c_, temp_max_c_);
    }
    const uint8_t step = celsius_to_temp_step(clamped);
    const bool ok = ble_->set_temperature(clamped);
    if (ok) {
        ESP_LOGI(TAG, "Setpoint %.2fC -> BedJet step %u (%.1fC)",
                 clamped, step, temp_step_to_celsius(step));
    } else {
        ESP_LOGW(TAG, "Setpoint %.2fC -> step %u dropped (link down or command queue full)",
                 clamped, step);
    }
    return ok;
}

bool BedjetMatter::do_percent_setting(uint8_t percent)
{
    if (!ble_) {
        return false;
    }

    if (percent == 0) {
        // The BedJet has no "fan off" step - stopping the blower means going to
        // standby, which also stops the heater. There is no way to express that
        // split on this hardware, so surface it rather than pretending otherwise.
        ESP_LOGI(TAG, "Fan 0%% -> BedJet standby (fan-only off also stops heating)");
        fan_target_percent_ = 0;
        fan_target_valid_   = true;
        return ble_->send_button(BTN_OFF);
    }

    const int step = fan_step_from_percent(percent);
    if (step < 0) {
        return false;
    }

    fan_target_percent_ = fan_percent_from_step(static_cast<uint8_t>(step));
    fan_target_valid_   = true;

    // A stopped BedJet ignores SET_FAN, so bring it up in its fan-only mode first.
    if (device_mode_ == MODE_STANDBY) {
        ESP_LOGI(TAG, "BedJet is off; enabling fan-only mode before setting speed");
        ble_->send_button(BTN_COOL);
    }

    ESP_LOGI(TAG, "Fan %u%% -> BedJet step %d", fan_target_percent_, step);
    return ble_->set_fan_step(static_cast<uint8_t>(step));
}

bool BedjetMatter::do_fan_mode(uint8_t matter_fan_mode)
{
    return do_percent_setting(fan_mode_to_percent(matter_fan_mode));
}

// ---------------------------------------------------------------------------
// Matter callbacks (CHIP stack task)
// ---------------------------------------------------------------------------

esp_err_t BedjetMatter::matter_attribute_update_cb(
    esp_matter::attribute::callback_type_t callback_type,
    uint16_t endpoint_id, uint32_t cluster_id,
    uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    if (callback_type != esp_matter::attribute::PRE_UPDATE ||
        !g_matter || !g_matter->cmd_queue_ || val == nullptr) {
        return ESP_OK;
    }

    // A device-originated report of a writable attribute also arrives here; it is
    // not a controller request and must not become a command.
    if (g_matter->publishing_.load(std::memory_order_acquire)) {
        return ESP_OK;
    }

    // Never send a command back to the BedJet for an endpoint we did not create.
    if (endpoint_id != g_matter->thermo_ep_id_ &&
        endpoint_id != g_matter->fan_ep_id_) {
        return ESP_OK;
    }

    MatterCommand cmd{};
    if (cluster_id == CLUSTER_THERMOSTAT) {
        switch (attribute_id) {
        case ATTR_SYSTEM_MODE:
            cmd.kind  = MATTER_CMD_SYSTEM_MODE;
            cmd.value = val->val.u8;
            break;
        case ATTR_OCCUPIED_HEATING_SETPOINT:
        case ATTR_OCCUPIED_COOLING_SETPOINT:
            cmd.kind  = MATTER_CMD_SETPOINT;
            cmd.value = val->val.i16;
            break;
        default:
            // LocalTemperature is reported by the device only.
            return ESP_OK;
        }
    } else if (cluster_id == CLUSTER_FAN_CONTROL) {
        switch (attribute_id) {
        case ATTR_PERCENT_SETTING:
            cmd.kind  = MATTER_CMD_PERCENT_SETTING;
            cmd.value = val->val.u8;
            break;
        case ATTR_FAN_MODE:
            cmd.kind  = MATTER_CMD_FAN_MODE;
            cmd.value = val->val.u8;
            break;
        default:
            // PercentCurrent and FanModeSequence are outputs.
            return ESP_OK;
        }
    } else {
        return ESP_OK;
    }

    if (xQueueSend(g_matter->cmd_queue_, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Matter command queue full, dropping write to cl=%" PRIX32 " at=%" PRIX32,
                 cluster_id, attribute_id);
    }
    return ESP_OK;
}

esp_err_t BedjetMatter::matter_identification_cb(
    esp_matter::identification::callback_type_t callback_type,
    uint16_t endpoint_id, uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Pairing
// ---------------------------------------------------------------------------

void BedjetMatter::print_pairing_info() const
{
    // PASE commissioning happens over BLE (CHIPoBLE). The rendezvous flag
    // describes the commissioning transport, not the operational network, so
    // it must be kBLE even though this is a Thread-only operational device.
    // Advertising kThread-only makes Apple Home try Thread DNS-SD discovery
    // against the unprovisioned "OpenThread-ESP" network and hang.
    constexpr auto kFlags = chip::BitFlags<chip::RendezvousInformationFlag, uint8_t>(
        chip::RendezvousInformationFlag::kBLE);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "           MATTER PAIRING INFORMATION");
    ESP_LOGI(TAG, "==================================================");

    // The 11-digit manual pairing code is the reliable option: it can be typed
    // into any controller, whereas a phone can only scan an actual QR image.
    char manual[16] = {};
    chip::MutableCharSpan manual_span(manual, sizeof(manual));
    if (GetManualPairingCode(manual_span, kFlags) == CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "  Manual pairing code: %.*s",
                 static_cast<int>(manual_span.size()), manual);
    } else {
        ESP_LOGE(TAG, "  Failed to build the manual pairing code");
    }

    // The QR payload is the same string the controller expects to scan. It is
    // printed rather than rendered because rendering needs an extra component.
    char qr[256] = {};
    chip::MutableCharSpan qr_span(qr, sizeof(qr));
    if (GetQRCode(qr_span, kFlags) == CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "  QR payload: %.*s",
                 static_cast<int>(qr_span.size()), qr);

        char url[336] = {};
        if (GetQRCodeUrl(url, sizeof(url), qr_span) == CHIP_NO_ERROR) {
            ESP_LOGI(TAG, "  QR URL: %s", url);
        }
    } else {
        ESP_LOGE(TAG, "  Failed to build the QR payload");
    }

    ESP_LOGI(TAG, "  Thermostat endpoint: %u, fan endpoint: %u", thermo_ep_id_, fan_ep_id_);
    ESP_LOGI(TAG, "==================================================");
}

} // namespace bedjet