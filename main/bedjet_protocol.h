#pragma once

#include <stdint.h>
#include <stdbool.h>

namespace bedjet {

// ---- BLE UUIDs (BedJet V3 custom service) ----
#define BEDJET_SERVICE_UUID   "00001000-bed0-0080-aa55-4265644a6574"
#define BEDJET_STATUS_UUID    "00002000-bed0-0080-aa55-4265644a6574"
#define BEDJET_COMMAND_UUID   "00002004-bed0-0080-aa55-4265644a6574"
#define BEDJET_NAME_UUID      "00002001-bed0-0080-aa55-4265644a6574"

// ---- Commands ----
enum BedjetCommand : uint8_t {
    CMD_BUTTON       = 0x01,
    CMD_SET_RUNTIME  = 0x02,
    CMD_SET_TEMP     = 0x03,
    CMD_STATUS       = 0x06,
    CMD_SET_FAN      = 0x07,
    CMD_SET_CLOCK    = 0x08,
};

// ---- Button values ----
enum BedjetButton : uint8_t {
    BTN_OFF          = 0x01,
    BTN_COOL         = 0x02,
    BTN_HEAT         = 0x03,
    BTN_TURBO        = 0x04,
    BTN_DRY          = 0x05,
    BTN_EXTHT        = 0x06,
    BTN_M1           = 0x20,
    BTN_M2           = 0x21,
    BTN_M3           = 0x22,
    MAGIC_NOTIFY_ACK = 0x52,
};

// ---- Operating modes ----
enum BedjetMode : uint8_t {
    MODE_STANDBY = 0,
    MODE_HEAT    = 1,
    MODE_TURBO   = 2,
    MODE_EXTHT   = 3,
    MODE_COOL    = 4,
    MODE_DRY     = 5,
    MODE_WAIT    = 6,
};

// ---- Packet format identifiers ----
enum BedjetPacketFormat : uint8_t {
    PACKET_FORMAT_DEBUG   = 0x05,
    PACKET_FORMAT_V3_HOME = 0x56,
};

enum BedjetPacketType : uint8_t {
    PACKET_TYPE_STATUS = 0x01,
    PACKET_TYPE_DEBUG  = 0x02,
};

// ---- Status packet layout ----
struct __attribute__((packed)) BedjetStatusPacket {
    uint8_t is_partial;
    uint8_t packet_format;
    uint8_t expecting_length;
    uint8_t packet_type;
    uint8_t time_remaining_hrs;
    uint8_t time_remaining_mins;
    uint8_t time_remaining_secs;
    uint8_t actual_temp_step;
    uint8_t target_temp_step;
    uint8_t mode;
    uint8_t fan_step;
    uint8_t max_hrs;
    uint8_t max_mins;
    uint8_t min_temp_step;
    uint8_t max_temp_step;
    uint16_t turbo_time;
    uint8_t ambient_temp_step;
};

// ---- Temperature conversion ----
inline float temp_step_to_celsius(uint8_t step) {
    return step / 2.0f;
}

inline uint8_t celsius_to_temp_step(float celsius) {
    return (uint8_t)(celsius * 2.0f);
}

// ---- Fan speed conversion ----
inline uint8_t fan_step_from_percent(uint8_t percent) {
    if (percent < 5)  return 0;
    if (percent > 100) return 19;
    return (percent / 5) - 1;
}

inline uint8_t fan_percent_from_step(uint8_t step) {
    return 5 * (step + 1);
}

// ---- Command packet builder ----
struct BedjetPacket {
    uint8_t command;
    uint8_t data[4];
    uint8_t data_len;

    static BedjetPacket button(BedjetButton btn) {
        return { CMD_BUTTON, {btn, 0, 0, 0}, 1 };
    }

    static BedjetPacket set_temp(float celsius) {
        return { CMD_SET_TEMP, {celsius_to_temp_step(celsius), 0, 0, 0}, 1 };
    }

    static BedjetPacket set_fan(uint8_t fan_step) {
        return { CMD_SET_FAN, {fan_step, 0, 0, 0}, 1 };
    }

    static BedjetPacket set_clock(uint8_t hrs, uint8_t mins) {
        return { CMD_SET_CLOCK, {hrs, mins, 0, 0}, 2 };
    }

    static BedjetPacket request_status() {
        return { CMD_STATUS, {0, 0, 0, 0}, 0 };
    }
};

} // namespace bedjet
