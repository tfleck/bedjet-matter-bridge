#pragma once

#include <atomic>
#include <functional>
#include <stdint.h>

#include "bedjet_protocol.h"

#include "NimBLEClient.h"
#include "NimBLEDevice.h"
#include "NimBLERemoteCharacteristic.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace bedjet {

using StatusCallback     = std::function<void(const BedjetNotification&)>;
using ConnStateCallback  = std::function<void(bool connected)>;
// Device-originated prompt code from the 11-byte status read (BedjetNotificationCode).
// Fired on change only, from the BLE task inside read_device_status().
using NotifyCallback     = std::function<void(uint8_t code)>;

class BedjetClientCallbacks : public NimBLEClientCallbacks {
public:
    void onConnect(NimBLEClient* pClient) override;
    void onConnectFail(NimBLEClient* pClient, int reason) override;
    void onDisconnect(NimBLEClient* pClient, int reason) override;
};

// Owns the NimBLE central role, the GATT link to the BedJet, and the reconnect
// loop.
//
// Threading model:
//   * ble_task_ is the only task that calls NimBLE connect / read / write and the
//     BLE scanner. Those calls block (GATT reads wait on the NimBLE host task), so
//     they must never run on the CHIP stack task.
//   * send_*() are safe to call from any task, including the CHIP stack task.
//     They only enqueue; they never perform I/O.
//   * post_notification() runs on the NimBLE host task. It parses and forwards the
//     result without blocking.
class BedjetBLE {
public:
    bool init();

    // Thread-safe and non-blocking. These return false only when there is no link
    // to send on, or the queue is full.
    bool send_button(BedjetButton btn);
    bool set_temperature(float celsius);
    bool set_fan_step(uint8_t step);

    bool is_connected() const { return connected_.load(std::memory_order_acquire); }

    // True once this driver has brought up its own NimBLE host. The boot
    // handover uses this to tell "Matter still owns the shared nimble_host
    // task" apart from "we already own it and only need to finish starting",
    // which matters when a retry follows a partial init().
    bool host_ready() const { return nimble_ready_; }

    // Called from the NimBLE host task via the subscription callback.
    void post_notification(const uint8_t* data, uint16_t len);

    void set_connected_state(bool connected);

    void on_status(StatusCallback cb)           { status_cb_  = std::move(cb); }
    void on_conn_state(ConnStateCallback cb)    { conn_cb_    = std::move(cb); }
    void on_notify(NotifyCallback cb)           { notify_cb_  = std::move(cb); }

private:
    friend class BedjetClientCallbacks;

    static void task_entry(void* arg);
    void run();

    // Disconnect the GAP link and drop any cached GATT handles. Used when
    // post-connect setup fails so the next link_once() cannot reuse a zombie
    // connection through its isConnected() fast path.
    void drop_link();

    bool enqueue(const BedjetPacket& pkt);

    bool load_mac_from_nvs();
    void store_mac_to_nvs();
    // Active scan when there is no usable saved address to match against
    // (first-ever discovery, or after the saved MAC was discarded as dead);
    // passive scan only for recovery while a saved address is still trusted.
    bool discover_device(bool active_scan);

    bool link_once();
    void teardown_link();
    bool write_packet(const BedjetPacket& pkt);
    // Returns true on a successful 11-byte flags read. Used as link liveness:
    // the BedJet is silent in standby, so a working poll must refresh the
    // notify watchdog or an idle device would be recycled every few minutes.
    bool read_device_status();
    // (Re)subscribe to status notifies. Returns false when the cached handles
    // went stale, in which case the caller must rediscover the database.
    bool subscribe_status();

    static constexpr uint32_t CMD_QUEUE_LEN  = 16;
    static constexpr uint32_t BACKOFF_MIN_MS = 1000;
    static constexpr uint32_t BACKOFF_MAX_MS = 30000;
    // Poll the status characteristic on this cadence. The read is what makes the
    // BedJet emit a fresh 20-byte state notification, so it is also how the
    // bridge picks up changes made on the device itself (e.g. the RF remote,
    // which never goes through us). 15 s mirrors ha-bedjet's coordinator.
    static constexpr uint32_t STATUS_POLL_MS = 15000;
    // Upper bound on silence from BOTH the 20-byte status notify and the status
    // poll before the link is assumed dead. The BedJet only pushes state on
    // change, so an idle device is silent for long stretches: a short
    // notify-only deadline false-triggered on every idle period and caused a
    // reconnect storm. A successful status read counts as liveness, so the poll
    // keeps a healthy idle link alive while this still catches a genuinely
    // wedged link. STATUS_POLL_FAILURES is the faster tripwire.
    static constexpr uint32_t NOTIFY_WATCHDOG_MS = 300000;
    // Consecutive failed status polls tolerated before recycling the link
    // (~45 s even if the watchdog above has not elapsed).
    static constexpr int STATUS_POLL_FAILURES = 3;
    static constexpr uint32_t SCAN_SECONDS   = 10;
    static constexpr uint32_t SCAN_RETRY_MS  = 1000;
    // Connect attempts per address before falling back to a rescan.
    static constexpr int CONNECT_ATTEMPTS = 3;
    // Consecutive link failures tolerated against a known address before the
    // saved MAC is discarded and discovery runs again.
    static constexpr int MAX_SAVED_ADDR_FAILURES = 3;
    // GATT write attempts per command packet and the pacing gap that follows
    // a successful write.
    static constexpr int      WRITE_RETRIES  = 3;
    static constexpr uint32_t WRITE_GAP_MS   = 100;

    std::atomic<bool> connected_{false};
    std::atomic<bool> has_device_{false};

    uint8_t target_mac_[6]{};
    uint8_t target_addr_type_{BLE_ADDR_PUBLIC};

    NimBLEClient*               p_client_{nullptr};
    NimBLERemoteCharacteristic* p_cmd_char_{nullptr};
    NimBLERemoteCharacteristic* p_status_char_{nullptr};
    // True when the command characteristic requires write-with-response.
    // Selected at link time from canWrite()/canWriteNoResponse() to match what
    // Bleak does (ha-bedjet never forces a response mode).
    bool cmd_write_response_{true};

    QueueHandle_t cmd_queue_{nullptr};
    TaskHandle_t  task_{nullptr};
    // init() is idempotent: the boot handover retries it, so the queue is
    // created once and NimBLEDevice::init()/xTaskCreate() each run at most once.
    bool nimble_ready_{false};
    bool initialized_{false};

    uint32_t backoff_ms_{BACKOFF_MIN_MS};
    // Consecutive link_once() failures against the current target_mac_.
    int saved_failures_{0};
    // True while target_mac_ is a trusted saved address. Cleared when the saved
    // MAC is discarded, which forces the next recovery scan to be active.
    bool have_saved_addr_{false};
    bool logged_first_link_{false};
    // Tracks whether the current link resolved its GATT database. Always
    // re-discovered per link (connect deleteAttributes=true) because the
    // client's service objects are deleted on disconnect; kept as a flag so
    // drop_link()/subscribe_status() can force the next link to treat the
    // database as unknown.
    bool gatt_cached_{false};
    // Tick count of the last proven-alive moment: a valid 20-byte status
    // notification OR a successful 11-byte flags read. Written from the NimBLE
    // host task (post_notification) and the BLE task (run), read from the BLE
    // task. Both writers use notify_mux_ so the reader never tears a 32-bit
    // read on this RISC-V core; staleness comparison is wrap-safe subtraction.
    uint32_t last_notify_ticks_{0};
    portMUX_TYPE notify_mux_ = portMUX_INITIALIZER_UNLOCKED;

    StatusCallback     status_cb_;
    ConnStateCallback  conn_cb_;
    NotifyCallback     notify_cb_;
    // Last notify code forwarded, so the callback fires only on a change. 0xFF
    // is not a valid BedjetNotificationCode, so the first read always reports.
    uint8_t            last_notify_code_{0xFF};
};

} // namespace bedjet