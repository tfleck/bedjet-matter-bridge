#pragma once

#include "bedjet_ble.h"
#include "bedjet_protocol.h"

#include <atomic>
#include <stdint.h>

#include <esp_matter.h>
#include <esp_matter_endpoint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace bedjet {

// Commands handed from the CHIP stack task to the bridge task. The CHIP task
// must never perform BLE I/O, and the bridge task owns all mutable state, so
// requests cross this queue instead of shared variables.
enum MatterCommandKind : uint8_t {
    MATTER_CMD_SYSTEM_MODE,
    MATTER_CMD_SETPOINT,
    MATTER_CMD_PERCENT_SETTING,
    MATTER_CMD_FAN_MODE,
};

struct MatterCommand {
    uint8_t kind;
    int32_t value;
};

// Exposes the BedJet as a Matter bridge:
//
//   endpoint 0  Root Node + Aggregator   NodeLabel "BedJet Matter Bridge"
//   endpoint 1  Bridged Node + Room Air Conditioner  NodeLabel "BedJet"
//               (On/Off + Thermostat + Fan Control all on one child accessory)
//   endpoint 2  Bridged Node + Contact Sensor        NodeLabel "BedJet Filter"
//
// Fan Control is not a legal cluster on a Thermostat endpoint (device type
// 0x0301), so the thermostat and fan cannot be two application device types on
// one endpoint. The Room Air Conditioner device type (0x0072) bundles On/Off +
// Thermostat as mandatory clusters and Fan Control as an optional cluster, so
// declaring that single device type keeps the thermostat and fan controls on
// one accessory, exactly as before, while BridgedDeviceBasicInformation adds a
// per-accessory Reachable bit.
//
// Threading model:
//   * post_status() runs on the NimBLE host task and only overwrites a
//     single-slot queue, so it never blocks and never queues stale state.
//   * A bridge task owns every Matter attribute write and all mutable state, so
//     the CHIP stack is never entered from a BLE callback context.
class BedjetMatter {
public:
    bool init(BedjetBLE* ble);

    // Thread-safe, non-blocking. Callable from the NimBLE host task.
    void post_status(const BedjetNotification& n);

    // Thread-safe, non-blocking. Callable from the BLE task. Delivers a BedJet
    // prompt code (BedjetNotificationCode) for the filter BooleanState.
    void post_notify_code(uint8_t code);

    // Thread-safe, non-blocking. Callable from the BLE task. Mirrors the BedJet
    // BLE link state into BridgedDeviceBasicInformation.Reachable on both
    // bridged endpoints, so a controller renders the accessory unreachable when
    // the BedJet link is down while the (Thread-connected) bridge stays up.
    void post_conn_state(bool connected);

    // Called once esp_matter::start() has returned. Applies the served
    // NodeLabel (see apply_identity): the node_config label is only the
    // create-time default, while the provider serves the NVS-persisted copy.
    void mark_matter_started();

    // Prints the Matter QR payload and manual pairing code.
    void print_pairing_info() const;

    uint16_t accessory_endpoint_id() const { return main_ep_id_; }
    uint16_t filter_endpoint_id() const { return filter_ep_id_; }

private:
    static void bridge_task_entry(void* arg);
    void run_bridge();

    void apply_identity();
    void apply_status(const BedjetNotification& n);
    void apply_filter_notify(uint8_t code);
    void execute_command(const MatterCommand& cmd);
    void learn_temp_limits(const BedjetNotification& n);

    bool do_system_mode(uint8_t matter_system_mode);
    bool do_setpoint(int16_t centi_degrees);
    bool do_percent_setting(uint8_t percent);
    bool do_fan_mode(uint8_t matter_fan_mode);

    // Declares the Bridged Node device type on an endpoint and attaches its
    // BridgedDeviceBasicInformation cluster (Reachable + NodeLabel). Shared by
    // the BedJet and filter child endpoints.
    bool add_bridged_node(esp_matter::endpoint_t* endpoint, const char* label);

    float clamp_temp(float celsius) const;
    int16_t centi_from_temp(float celsius) const;
    static int16_t centi_from_step(uint8_t step);

    // Each returns true when the value published. Callers only advance their
    // change-detection baseline on success so a failed report is retried on
    // the next status instead of being lost until the value changes again.
    bool publish_enum8(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, uint8_t value);
    bool publish_u8(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, uint8_t value);
    bool publish_nullable_u8(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, uint8_t value);
    bool publish_nullable_i16(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, int16_t value);
    bool publish_i16(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, int16_t value);
    bool publish_bool(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, bool value);
    bool publish_reachable(bool reachable);

    static uint8_t  bedjet_mode_to_system_mode(uint8_t mode);
    static int      system_mode_to_button(uint8_t matter_system_mode);
    static uint8_t  percent_to_fan_mode(uint8_t percent);
    static uint8_t  fan_mode_to_percent(uint8_t matter_fan_mode);

    BedjetBLE*           ble_{nullptr};
    esp_matter::node_t*     node_{nullptr};
    esp_matter::endpoint_t* root_ep_{nullptr};
    esp_matter::endpoint_t* main_ep_{nullptr};
    esp_matter::endpoint_t* filter_ep_{nullptr};

    uint16_t main_ep_id_{0};
    uint16_t filter_ep_id_{0};

    QueueHandle_t status_queue_{nullptr};
    QueueHandle_t cmd_queue_{nullptr};
    QueueHandle_t notify_queue_{nullptr};
    QueueHandle_t conn_queue_{nullptr};
    QueueSetHandle_t queue_set_{nullptr};
    TaskHandle_t  task_{nullptr};

    // Owned exclusively by the bridge task.
    uint8_t device_mode_{MODE_STANDBY};
    uint8_t fan_step_{0};
    uint8_t fan_target_percent_{0};
    bool    fan_target_valid_{false};
    float   temp_min_c_{BEDJET_TEMP_MIN_C};
    float   temp_max_c_{BEDJET_TEMP_MAX_C};

    // Change detection, so a steady-state notification poll does not spam logs
    // or mark every attribute dirty.
    uint8_t  last_logged_mode_{0xFF};
    uint8_t  last_logged_step_{0xFF};
    int16_t  last_logged_target_{-32768};
    int16_t  last_local_centi_{INT16_MIN};
    int16_t  last_heat_centi_{INT16_MIN};
    int16_t  last_cool_centi_{INT16_MIN};
    uint8_t  last_system_mode_{0xFF};
    uint8_t  last_percent_current_{0xFF};
    uint8_t  last_percent_setting_{0xFF};
    uint8_t  last_fan_mode_{0xFF};
    // Room Air Conditioner OnOff (mandatory) mirrors "BedJet is running".
    uint8_t  last_onoff_{0xFF};
    // Filter BooleanState (StateValue) change detection. 0xFF means "nothing
    // published yet", so the first notify code always lands.
    uint8_t  last_filter_state_{0xFF};
    // BridgedDeviceBasicInformation.Reachable change detection. Starts at 0xFF
    // so the first BLE conn-state event always publishes.
    uint8_t  last_reachable_{0xFF};

    // Set for the duration of a single device-originated report() call while
    // the bridge task is publishing. Scoped per report (not per apply_status)
    // so a controller write that lands between two reports is still honoured.
    // See PublishGuard in bedjet_matter.cpp for why it is required.
    std::atomic<bool> publishing_{false};

    static esp_err_t matter_attribute_update_cb(
        esp_matter::attribute::callback_type_t callback_type,
        uint16_t endpoint_id, uint32_t cluster_id,
        uint32_t attribute_id, esp_matter_attr_val_t* val, void* priv_data);

    static esp_err_t matter_identification_cb(
        esp_matter::identification::callback_type_t callback_type,
        uint16_t endpoint_id, uint8_t effect_id,
        uint8_t effect_variant, void* priv_data);
};

extern BedjetMatter* g_matter;

} // namespace bedjet