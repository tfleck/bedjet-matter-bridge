#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

// BedJet V3 over BLE.
//
// Command opcodes, payload shapes and the notification/status layouts below were
// cross-checked against the ha-bedjet reference implementation
// (custom_components/bedjet/pybedjet). BedJet V3 commands are written raw to the
// command characteristic - there is no checksum, prefix or padding.

// ---- GATT UUIDs (BedJet V3) ----
#define BEDJET_SERVICE_UUID  "00001000-bed0-0080-aa55-4265644a6574"
#define BEDJET_STATUS_UUID   "00002000-bed0-0080-aa55-4265644a6574"
#define BEDJET_NAME_UUID     "00002001-bed0-0080-aa55-4265644a6574"
#define BEDJET_COMMAND_UUID  "00002004-bed0-0080-aa55-4265644a6574"

namespace bedjet {

// The status characteristic carries two different payloads:
//  - pushed as a notification: the full 20 byte state block
//  - returned by a GATT read:   an 11 byte flags block
constexpr size_t BEDJET_NOTIFICATION_LEN = 20;
constexpr size_t BEDJET_STATUS_READ_LEN  = 11;
constexpr size_t BEDJET_NAME_LEN         = 32;

// Device-reported temperature limits, in degrees Celsius. Refined at runtime from
// BedjetNotification::min_temp_step / max_temp_step, which are authoritative.
constexpr float BEDJET_TEMP_MIN_C = 19.0f;
constexpr float BEDJET_TEMP_MAX_C = 43.0f;

// The BedJet exposes 20 discrete fan steps (5% granularity). Step 0 is the
// lowest *running* speed (5%); stopping the fan entirely is a mode change to
// standby, not a fan step, so 0% has no corresponding step.
constexpr uint8_t BEDJET_FAN_STEP_COUNT = 20;
constexpr uint8_t BEDJET_FAN_STEP_MAX   = BEDJET_FAN_STEP_COUNT - 1;
constexpr uint8_t BEDJET_FAN_PERCENT_MIN = 5;
constexpr uint8_t BEDJET_FAN_PERCENT_MAX = 100;

// ---- Commands ----
enum BedjetCommand : uint8_t {
    CMD_BUTTON          = 0x01,
    CMD_SET_TEMPERATURE = 0x03,
    CMD_SET_FAN         = 0x07,
};

// ---- Buttons ----
enum BedjetButton : uint8_t {
    BTN_OFF   = 0x01,
    BTN_COOL  = 0x02,
    BTN_HEAT  = 0x03,
    BTN_TURBO = 0x04,
    BTN_DRY   = 0x05,
    BTN_EXTHT = 0x06,
    BTN_M1    = 0x20,
    BTN_M2    = 0x21,
    BTN_M3    = 0x22,
    BTN_NOTIFY_ACK = 0x52,
};

// ---- Operating modes (BedjetNotification::mode) ----
enum BedjetMode : uint8_t {
    MODE_STANDBY = 0,
    MODE_HEAT    = 1,
    MODE_TURBO   = 2,
    MODE_EXTHT   = 3,
    MODE_COOL    = 4,
    MODE_DRY     = 5,
    MODE_WAIT    = 6,
};

// ---- Status notification messages (BedjetDeviceStatus::notification) ----
enum BedjetNotificationCode : uint8_t {
    NOTIFY_NONE                = 0,
    NOTIFY_CLEAN_FILTER        = 1,
    NOTIFY_UPDATE_AVAILABLE    = 2,
    NOTIFY_UPDATE_FAILED       = 3,
    NOTIFY_BIO_FAIL_CLOCK      = 4,
    NOTIFY_BIO_FAIL_TOO_LONG   = 5,
};

// ---- 20 byte notification pushed on the status characteristic ----
// Bytes 0-3 are undocumented in the reference implementation; byte 2 bit 1 is
// the dual-zone flag. Bytes 18-19 are the shutdown reason and one unknown byte.
struct __attribute__((packed)) BedjetNotification {
    uint8_t  flags0;           // [0]  undocumented
    uint8_t  flags1;           // [1]  undocumented
    uint8_t  status_bits;      // [2]  bit 1 = dual zone
    uint8_t  flags3;           // [3]  undocumented
    uint8_t  runtime_hrs;      // [4]
    uint8_t  runtime_mins;     // [5]
    uint8_t  runtime_secs;     // [6]
    uint8_t  actual_temp_step; // [7]  outlet air temp, degrees C * 2
    uint8_t  target_temp_step; // [8]  setpoint, degrees C * 2
    uint8_t  mode;             // [9]  BedjetMode
    uint8_t  fan_step;         // [10] 0..19
    uint8_t  max_runtime_hrs;  // [11]
    uint8_t  max_runtime_mins; // [12]
    uint8_t  min_temp_step;    // [13] degrees C * 2
    uint8_t  max_temp_step;    // [14] degrees C * 2
    uint8_t  turbo_time_hi;    // [15] turbo seconds, big endian (MSB first)
    uint8_t  turbo_time_lo;    // [16] turbo seconds, big endian (LSB)
    uint8_t  ambient_temp_step;// [17] room air temp, degrees C * 2 (climate current-temp source)
    uint8_t  shutdown_reason;  // [18]
    uint8_t  flags19;          // [19] undocumented
};
static_assert(sizeof(BedjetNotification) == BEDJET_NOTIFICATION_LEN,
              "BedjetNotification must match the 20 byte V3 notification");

// Turbo countdown in seconds. Bytes 15-16 are big-endian on the wire; the
// struct keeps them split so no unaligned or wrong-endian uint16_t read can
// happen on the little-endian RISC-V core.
inline uint16_t turbo_time_seconds(const BedjetNotification &n)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(n.turbo_time_hi) << 8) |
                                 static_cast<uint16_t>(n.turbo_time_lo));
}

// ---- 11 byte block returned by reading the status characteristic ----
struct __attribute__((packed)) BedjetDeviceStatus {
    uint8_t byte0;         // [0] undocumented
    uint8_t byte1;         // [1] undocumented
    uint8_t status_bits;   // [2] bit 1 = dual zone
    uint8_t byte3;         // [3] undocumented
    uint8_t byte4;         // [4] undocumented
    uint8_t byte5;         // [5] undocumented
    uint8_t update_phase;  // [6] firmware update progress
    uint8_t flags;         // [7] see bit masks below
    uint8_t bio_step;      // [8] biorhythm sequence step
    uint8_t notification;  // [9] BedjetNotificationCode
    uint8_t byte10;        // [10] undocumented
};
static_assert(sizeof(BedjetDeviceStatus) == BEDJET_STATUS_READ_LEN,
              "BedjetDeviceStatus must match the 11 byte V3 status read");

constexpr uint8_t STATUS_BIT_DUAL_ZONE       = 1 << 1;
constexpr uint8_t STATUS_FLAG_TEST_PASSED    = 1 << 5;
constexpr uint8_t STATUS_FLAG_LED_ENABLED     = 1 << 4;
constexpr uint8_t STATUS_FLAG_UNITS_SETUP    = 1 << 2;
constexpr uint8_t STATUS_FLAG_BEEPS_MUTED     = 1 << 0;

// ---- Temperature conversion ----
// Wire resolution is half-degree steps: step N is exactly N/2 degrees C.
// temp_step_to_celsius is exact for all steps (N/2 is representable for N<256
// in float, and the division is exact). celsius_to_temp_step rounds half away
// from zero to the nearest step so Matter/HomeKit Fahrenheit setpoints snap to
// the closest achievable device temperature instead of truncating up to half a
// degree low (which reads back 1 F under the radio remote on steps whose exact
// Fahrenheit fraction is >= .5).
inline float temp_step_to_celsius(uint8_t step) { return step / 2.0f; }

inline uint8_t celsius_to_temp_step(float celsius)
{
    return static_cast<uint8_t>(lroundf(celsius * 2.0f));
}

// ---- Fan speed conversion ----
// The BedJet reports a fan step; Matter works in whole percent.
inline uint8_t fan_percent_from_step(uint8_t step)
{
    if (step > BEDJET_FAN_STEP_MAX) step = BEDJET_FAN_STEP_MAX;
    return static_cast<uint8_t>(BEDJET_FAN_STEP_COUNT * (step + 1) / 4);
}

// Returns the BedJet fan step for a Matter percentage, or -1 when the requested
// percentage means "off" (which the BedJet models as a mode change, not a step).
// Values between 1% and 4% snap up to the BedJet's lowest running speed of 5%.
inline int fan_step_from_percent(uint8_t percent)
{
    if (percent == 0) return -1;
    if (percent < BEDJET_FAN_PERCENT_MIN) percent = BEDJET_FAN_PERCENT_MIN;
    if (percent > BEDJET_FAN_PERCENT_MAX) percent = BEDJET_FAN_PERCENT_MAX;
    return static_cast<int>(percent) / 5 - 1;
}

// ---- Command packet ----
struct BedjetPacket {
    uint8_t bytes[6] = {};
    uint8_t len      = 0;
};

inline BedjetPacket make_button_packet(BedjetButton btn)
{
    BedjetPacket p{};
    p.bytes[0] = CMD_BUTTON;
    p.bytes[1] = static_cast<uint8_t>(btn);
    p.len      = 2;
    return p;
}

inline BedjetPacket make_temp_packet(float celsius)
{
    BedjetPacket p{};
    // Defensive clamp at the BLE layer: Matter clamps too, but a direct
    // caller must never push an out-of-range step onto the wire.
    if (celsius < BEDJET_TEMP_MIN_C) celsius = BEDJET_TEMP_MIN_C;
    if (celsius > BEDJET_TEMP_MAX_C) celsius = BEDJET_TEMP_MAX_C;
    p.bytes[0] = CMD_SET_TEMPERATURE;
    p.bytes[1] = celsius_to_temp_step(celsius);
    p.len      = 2;
    return p;
}

inline BedjetPacket make_fan_packet(uint8_t step)
{
    BedjetPacket p{};
    p.bytes[0] = CMD_SET_FAN;
    p.bytes[1] = step > BEDJET_FAN_STEP_MAX ? BEDJET_FAN_STEP_MAX : step;
    p.len      = 2;
    return p;
}

} // namespace bedjet