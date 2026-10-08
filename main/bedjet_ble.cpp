#include "bedjet_ble.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <inttypes.h>
#include <string>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "bedjet_ble";

namespace bedjet {

static const char *NVS_NAMESPACE = "bedjet";
static const char *NVS_KEY_MAC    = "mac";

static BedjetBLE *g_instance = nullptr;

namespace {

void log_mac(const char *what, const uint8_t mac[6])
{
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X", what,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The BedJet advertises with a human readable local name; fall back to the
// vendor service UUID for units that omit or truncate it.
bool looks_like_bedjet(const NimBLEAdvertisedDevice &dev)
{
    if (to_lower(dev.getName()).find("bedjet") != std::string::npos) {
        return true;
    }
    const NimBLEUUID want(BEDJET_SERVICE_UUID);
    for (uint8_t i = 0; i < dev.getServiceUUIDCount(); ++i) {
        if (dev.getServiceUUID(i).equals(want)) {
            return true;
        }
    }
    return false;
}

} // namespace

void BedjetClientCallbacks::onConnect(NimBLEClient *pClient)
{
    if (pClient && g_instance) {
        ESP_LOGD(TAG, "Link established, resolving GATT services (rssi=%d)",
                 pClient->getRssi());
    } else {
        ESP_LOGD(TAG, "Link established, resolving GATT services");
    }
    // connected_ is deliberately not raised here: the link is not usable until
    // link_once() has resolved the characteristics and subscribed to status.
}

void BedjetClientCallbacks::onConnectFail(NimBLEClient *pClient, int reason)
{
    ESP_LOGW(TAG, "Connect attempt failed, reason %d (%s)", reason,
             NimBLEUtils::returnCodeToString(reason));
    (void)pClient;
}

void BedjetClientCallbacks::onDisconnect(NimBLEClient *pClient, int reason)
{
    int rssi = 0;
    if (pClient && pClient->getConnHandle() != BLE_HS_CONN_HANDLE_NONE) {
        rssi = pClient->getRssi();
    }
    ESP_LOGW(TAG, "Disconnected from BedJet, reason %d last-rssi=%d", reason, rssi);
    if (g_instance) {
        g_instance->set_connected_state(false);
    }
}

bool BedjetBLE::init()
{
    // Idempotent: the boot handover calls this repeatedly until Matter releases
    // the shared NimBLE controller. Each step runs at most once, so a retry can
    // neither leak a second command queue nor double-init NimBLE or spawn a
    // duplicate task.
    if (initialized_) {
        return true;
    }
    g_instance = this;

    if (!cmd_queue_) {
        cmd_queue_ = xQueueCreate(CMD_QUEUE_LEN, sizeof(BedjetPacket));
        if (!cmd_queue_) {
            ESP_LOGE(TAG, "Failed to allocate command queue");
            return false;
        }
    }

    if (!nimble_ready_) {
        if (!NimBLEDevice::init("")) {
            // The controller is still owned by Matter. Keep the queue and retry
            // later; do NOT create the task until the host comes up.
            ESP_LOGE(TAG, "NimBLEDevice::init failed");
            return false;
        }
        nimble_ready_ = true;
    }

    if (!task_) {
        if (xTaskCreate(&BedjetBLE::task_entry, "bedjet_ble", 6144, this, 5, &task_) != pdPASS) {
            ESP_LOGE(TAG, "Failed to spawn BLE task");
            return false;
        }
        ESP_LOGI(TAG, "BLE task high-water mark at spawn: %u bytes",
                 uxTaskGetStackHighWaterMark(task_));
    }

    initialized_ = true;
    return true;
}

void BedjetBLE::task_entry(void *arg)
{
    static_cast<BedjetBLE *>(arg)->run();
}

void BedjetBLE::set_connected_state(bool connected)
{
    const bool was = connected_.exchange(connected, std::memory_order_acq_rel);
    if (was == connected) {
        return;
    }
    if (conn_cb_) {
        conn_cb_(connected);
    }
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------

bool BedjetBLE::load_mac_from_nvs()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    uint8_t blob[7] = {};
    size_t   len   = sizeof(blob);
    const esp_err_t err = nvs_get_blob(handle, NVS_KEY_MAC, blob, &len);
    nvs_close(handle);

    if (err != ESP_OK || len != sizeof(blob)) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Stored MAC unreadable: %s", esp_err_to_name(err));
        }
        return false;
    }

    std::memcpy(target_mac_, blob, 6);
    target_addr_type_ = blob[6];

    bool all_zero = true;
    for (int i = 0; i < 6; ++i) {
        if (target_mac_[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        ESP_LOGW(TAG, "Stored MAC is all-zeros, ignoring");
        return false;
    }

    log_mac("Using saved BedJet", target_mac_);
    have_saved_addr_ = true;
    return true;
}

void BedjetBLE::store_mac_to_nvs()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open NVS namespace '%s'", NVS_NAMESPACE);
        return;
    }

    uint8_t blob[7];
    std::memcpy(blob, target_mac_, 6);
    blob[6] = target_addr_type_;

    esp_err_t err = nvs_set_blob(handle, NVS_KEY_MAC, blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "BedJet address saved to NVS");
    } else {
        ESP_LOGW(TAG, "Failed to save BedJet address: %s", esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------------------
// Discovery (callback-driven, mirrors ha-bedjet's bleak discovery)
// ---------------------------------------------------------------------------

namespace {

struct ScanState {
    uint8_t  found_mac[6]{};
    uint8_t  found_addr_type{0};
    int8_t   found_rssi{-127};
    bool     found{false};
};

class BedjetScanCallbacks : public NimBLEScanCallbacks {
public:
    explicit BedjetScanCallbacks(ScanState *state) : state_(state) {}

    void onResult(const NimBLEAdvertisedDevice *dev) override
    {
        if (dev == nullptr || !dev->isConnectable() || !looks_like_bedjet(*dev)) {
            return;
        }
        // ha-bedjet matches the vendor service UUID or a local name starting
        // with BEDJET, then picks the strongest signal.
        if (!state_->found || dev->getRSSI() > state_->found_rssi) {
            const ble_addr_t *base = dev->getAddress().getBase();
            std::memcpy(state_->found_mac, base->val, 6);
            state_->found_addr_type = base->type;
            state_->found_rssi      = dev->getRSSI();
            state_->found           = true;
            ESP_LOGD(TAG, "  candidate '%s' %s rssi=%d",
                     dev->getName().c_str(),
                     dev->getAddress().toString().c_str(),
                     dev->getRSSI());
        }
    }

private:
    ScanState *state_;
};

} // namespace

bool BedjetBLE::discover_device(bool active_scan)
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan) {
        ESP_LOGE(TAG, "NimBLE scan instance unavailable");
        return false;
    }

    if (scan->isScanning()) {
        scan->stop();
    }

    ScanState state;
    BedjetScanCallbacks callbacks(&state);

    scan->clearResults();
    // Active scan requests scan-response packets (faster name match) but
    // occupies the radio at 100% duty cycle; passive scan only listens.
    // Recovery scans stay passive only while a saved address is still trusted
    // (cheap, leaves airtime for Matter/Thread). Once there is nothing to match
    // against - first-ever discovery, or a discarded dead MAC - the scan must
    // be active or a BedJet whose name lives in the scan response is invisible.
    scan->setActiveScan(active_scan);
    // Active scan runs at 50% duty: recovery now scans actively after a
    // discarded address, so keep half the airtime free for the Thread radio.
    // Passive listening can use the full window (it never transmits).
    scan->setInterval(100);
    scan->setWindow(active_scan ? 50 : 100);
    scan->setScanCallbacks(&callbacks, /*wantDuplicates=*/true);

    ESP_LOGI(TAG, "Scanning %" PRIu32 " s for a BedJet (%s)...", SCAN_SECONDS,
             active_scan ? "active" : "passive");
    // Blocking overload: starts the scan and releases the internal task handle
    // on completion. Results stream into onResult() above; a null return means
    // the controller ran out of memory and no results are valid.
    NimBLEScanResults results = scan->getResults(SCAN_SECONDS * 1000, /*is_continue=*/false);
    // getResults() returns an empty set both when nothing was found and when
    // the controller ran out of memory mid-scan; the callback state tells them
    // apart.
    if (results.getCount() == 0 && !state.found) {
        ESP_LOGD(TAG, "Scan returned no results");
    }
    // getResults(str) with wantDuplicates=true leaves duplicate filtering off;
    // restore controller dedup for the next cycle.
    scan->setScanCallbacks(nullptr, /*wantDuplicates=*/false);
    scan->clearResults();

    if (!state.found) {
        ESP_LOGW(TAG, "No BedJet found, retrying in %" PRIu32 " ms", SCAN_RETRY_MS);
        return false;
    }

    bool all_zero = true;
    for (int i = 0; i < 6; ++i) {
        if (state.found_mac[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        ESP_LOGW(TAG, "Discovered address is all-zeros, ignoring");
        return false;
    }

    // NimBLEAddress(uint8_t[6]) expects display order and reverses internally;
    // the scan copy above is controller (little-endian) order, so reverse it
    // into display order and let the constructor flip it back.
    for (int i = 0; i < 6; ++i) {
        target_mac_[i] = state.found_mac[5 - i];
    }
    target_addr_type_ = state.found_addr_type;

    NimBLEAddress resolved(target_mac_, target_addr_type_);
    ESP_LOGI(TAG, "Discovered BedJet %s rssi=%d",
             resolved.toString().c_str(), state.found_rssi);

    store_mac_to_nvs();
    have_saved_addr_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Link management (mirrors ha-bedjet's bleak_retry_connector establish flow)
// ---------------------------------------------------------------------------

void BedjetBLE::drop_link()
{
    p_cmd_char_    = nullptr;
    p_status_char_ = nullptr;
    gatt_cached_   = false;
    if (p_client_ && p_client_->isConnected()) {
        p_client_->disconnect();
        // Give the controller a moment to report the disconnect so the next
        // connect() does not race the teardown.
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

bool BedjetBLE::link_once()
{
    // Display-order target_mac_ from discovery/NVS; the NimBLEAddress ctor
    // reverses into controller order internally.
    NimBLEAddress addr(target_mac_, target_addr_type_);

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) {
        scan->stop();
    }

    // Reuse one client object and let connect() block. ha-bedjet's
    // establish_connection retries on a single BleakClient; deleting clients
    // mid-loop only leaks the peer-address slots that exhaust
    // BLE_MAX_CONNECTIONS after a few drops.
    NimBLEClient* client = p_client_;
    if (!client) {
        client = NimBLEDevice::createClient();
        if (!client) {
            // Slot table full of stale clients: reap disconnected ones once.
            for (int i = 0; i < 5; ++i) {
                NimBLEClient* stale = NimBLEDevice::getDisconnectedClient();
                if (!stale) {
                    break;
                }
                NimBLEDevice::deleteClient(stale);
            }
            client = NimBLEDevice::createClient();
        }
        if (!client) {
            ESP_LOGE(TAG, "NimBLEDevice::createClient failed (slots exhausted)");
            return false;
        }
        client->setClientCallbacks(new BedjetClientCallbacks(), true);
        p_client_ = client;
    }

    client->setConnectionParams(24, 40, 0, 400, 16, 16);
    // setConnectTimeout takes milliseconds (default 30000): 10 s per attempt.
    client->setConnectTimeout(10000);

    bool ok = false;
    for (int attempt = 1; attempt <= CONNECT_ATTEMPTS; ++attempt) {
        if (client->isConnected()) {
            ok = true;
            break;
        }
        // Always force a fresh service database. The BedJet's GATT table is
        // static, but the client's cached NimBLERemoteService objects are
        // owned by vectors that disconnect/delete recycles: holding our raw
        // characteristic pointers across a link is a use-after-free
        // (the reconnect panic at NimBLERemoteService::getCharacteristic).
        // Discovery costs ~1 s; correctness beats speed here.
        if (client->connect(addr, /*deleteAttributes=*/true)) {
            ok = true;
            break;
        }

        ESP_LOGW(TAG, "Connect to %s failed (attempt %d/%d): status=%d %s",
                 addr.toString().c_str(), attempt, CONNECT_ATTEMPTS,
                 client->getLastError(),
                 NimBLEUtils::returnCodeToString(client->getLastError()));

        if (attempt < CONNECT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    if (!ok) {
        return false;
    }

    // MTU is already exchanged by connect() (exchangeMTU=true default); ask
    // for Data Length Extension so the 20-byte notifies and 11-byte reads
    // each fit in fewer link-layer transactions.
    if (p_client_->getMTU() < 185) {
        p_client_->setDataLen(251);
    }

    NimBLERemoteService *svc = p_client_->getService(BEDJET_SERVICE_UUID);
    if (!svc) {
        ESP_LOGE(TAG, "BedJet service %s not present", BEDJET_SERVICE_UUID);
        drop_link();
        return false;
    }

    // connect(deleteAttributes=true) above rebuilt the client's database, so
    // these are fresh objects owned by the current link. Resolve them every
    // time; never hold them across teardown_link().
    p_cmd_char_    = svc->getCharacteristic(BEDJET_COMMAND_UUID);
    p_status_char_ = svc->getCharacteristic(BEDJET_STATUS_UUID);
    if (!p_cmd_char_ || !p_status_char_) {
        ESP_LOGE(TAG, "BedJet command/status characteristics not found");
        drop_link();
        return false;
    }
    gatt_cached_ = true;

    // ha-bedjet writes V3 commands with response (Bleak default); honour the
    // characteristic's declared properties rather than hard-coding it.
    cmd_write_response_ = p_cmd_char_->canWrite();
    ESP_LOGI(TAG, "Command characteristic props: write=%d write_nr=%d notify=%d",
             p_cmd_char_->canWrite() ? 1 : 0,
             p_cmd_char_->canWriteNoResponse() ? 1 : 0,
             p_cmd_char_->canNotify() ? 1 : 0);
    if (!p_cmd_char_->canWrite() && !p_cmd_char_->canWriteNoResponse()) {
        ESP_LOGE(TAG, "Command characteristic exposes no write property; commands will fail");
    }

    // The name characteristic is the authoritative device identity.
    if (NimBLERemoteCharacteristic *name_char = svc->getCharacteristic(BEDJET_NAME_UUID)) {
        NimBLEAttValue value = name_char->readValue();
        if (value.size() > 0) {
            char name[BEDJET_NAME_LEN] = {};
            const size_t n = std::min<size_t>(value.size(), BEDJET_NAME_LEN - 1);
            std::memcpy(name, value.data(), n);
            ESP_LOGI(TAG, "Reporting in as '%s'", name);
        }
    }

    if (p_status_char_->canNotify() || p_status_char_->canIndicate()) {
        if (!subscribe_status()) {
            ESP_LOGE(TAG, "Failed to subscribe to BedJet status notifications");
            drop_link();
            return false;
        }
    } else {
        ESP_LOGW(TAG, "Status characteristic cannot notify; state will be blind");
    }

    backoff_ms_ = BACKOFF_MIN_MS;
    set_connected_state(true);
    if (p_client_) {
        const int rssi = p_client_->getRssi();
        ESP_LOGI(TAG, "BedJet link ready (rssi=%d mtu=%u)", rssi, p_client_->getMTU());
        if (rssi < -85) {
            ESP_LOGW(TAG, "BedJet RSSI weak (%d dBm); expect drops", rssi);
        }
    }
    if (!logged_first_link_) {
        logged_first_link_ = true;
        ESP_LOGI(TAG, "BLE task high-water mark after first link: %u bytes",
                 uxTaskGetStackHighWaterMark(task_));
    }
    read_device_status();
    return true;
}

void BedjetBLE::teardown_link()
{
    set_connected_state(false);
    // Always drop the raw handles: connect(deleteAttributes=true) on the next
    // link deletes the service objects that own them, so any pointer held
    // across a teardown is a use-after-free (the reconnect panic).
    p_cmd_char_    = nullptr;
    p_status_char_ = nullptr;
    gatt_cached_   = false;

    // Commands queued while the link was down are stale by definition: the
    // BedJet state may have changed out from under them (app, remote, power
    // cycle), so drop them rather than firing them at the next link.
    if (cmd_queue_) {
        xQueueReset(cmd_queue_);
    }

    // Keep the client object for reuse; the cached database carries over to
    // the next link. Only reap it when the stack self-deleted it
    // (deleteOnConnectFail) or the link is truly gone.
    if (p_client_ && !p_client_->isConnected()) {
        NimBLEClient *still_there = NimBLEDevice::getClientByPeerAddress(
            NimBLEAddress(target_mac_, target_addr_type_));
        if (!still_there) {
            p_client_ = nullptr;
            gatt_cached_ = false;
        }
    }
}

bool BedjetBLE::subscribe_status()
{
    if (!p_status_char_) {
        return false;
    }
    const bool notify = p_status_char_->canNotify();
    const bool ok_sub = p_status_char_->subscribe(
        notify,
        [this](NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
            post_notification(data, static_cast<uint16_t>(length));
        },
        true);
    if (!ok_sub) {
        // Handles went stale (e.g. BedJet firmware changed its table).
        // drop_link() clears the cache; the next link rediscovers.
        ESP_LOGW(TAG, "Status subscribe failed, invalidating GATT cache");
        gatt_cached_   = false;
        p_cmd_char_    = nullptr;
        p_status_char_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "Subscribed to BedJet status %s",
             notify ? "notifications" : "indications");
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

bool BedjetBLE::enqueue(const BedjetPacket &pkt)
{
    if (!cmd_queue_ || !is_connected()) {
        return false;
    }
    if (xQueueSend(cmd_queue_, &pkt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropping 0x%02X", pkt.bytes[0]);
        return false;
    }
    return true;
}

bool BedjetBLE::send_button(BedjetButton btn)
{
    return enqueue(make_button_packet(btn));
}

bool BedjetBLE::set_temperature(float celsius)
{
    return enqueue(make_temp_packet(celsius));
}

bool BedjetBLE::set_fan_step(uint8_t step)
{
    return enqueue(make_fan_packet(step));
}

bool BedjetBLE::write_packet(const BedjetPacket &pkt)
{
    if (!p_cmd_char_ || !is_connected()) {
        return false;
    }
    for (int attempt = 1; attempt <= WRITE_RETRIES; ++attempt) {
        if (!is_connected()) {
            return false;
        }
        if (p_cmd_char_->writeValue(pkt.bytes, pkt.len, cmd_write_response_)) {
            ESP_LOGD(TAG, "TX 0x%02X %02X", pkt.bytes[0], pkt.len > 1 ? pkt.bytes[1] : 0);
            // The BedJet V3 drops back-to-back writes; pace commands so a
            // mode+temp+fan burst does not lose the tail.
            vTaskDelay(pdMS_TO_TICKS(WRITE_GAP_MS));
            return true;
        }
        ESP_LOGW(TAG, "GATT write of 0x%02X failed (attempt %d/%d)",
                 pkt.bytes[0], attempt, WRITE_RETRIES);
        if (attempt < WRITE_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(WRITE_GAP_MS));
        }
    }
    return false;
}

bool BedjetBLE::read_device_status()
{
    if (!p_status_char_ || !is_connected()) {
        return false;
    }

    NimBLEAttValue value = p_status_char_->readValue();
    if (value.size() != BEDJET_STATUS_READ_LEN) {
        ESP_LOGW(TAG, "Status read returned %u bytes, expected %u",
                 value.size(), static_cast<unsigned>(BEDJET_STATUS_READ_LEN));
        return false;
    }

    BedjetDeviceStatus st{};
    std::memcpy(&st, value.data(), sizeof(st));

    // Debug level: this fires every 60 s for the life of the device.
    ESP_LOGD(TAG,
             "Device flags: dual_zone=%d led=%d muted=%d units_setup=%d test=%d "
             "notify=%u bio_step=%u update_phase=%u",
             (st.status_bits & STATUS_BIT_DUAL_ZONE) ? 1 : 0,
             (st.flags & STATUS_FLAG_LED_ENABLED) ? 1 : 0,
             (st.flags & STATUS_FLAG_BEEPS_MUTED) ? 1 : 0,
             (st.flags & STATUS_FLAG_UNITS_SETUP) ? 1 : 0,
             (st.flags & STATUS_FLAG_TEST_PASSED) ? 1 : 0,
             st.notification, st.bio_step, st.update_phase);

    // The notify byte is a device-originated prompt (clean filter, firmware
    // update, biorhythm clock errors). It only exists in this 11-byte flags
    // read, not the 20-byte state notification, so surface it here. Forward on
    // change only: the caller mirrors it into Matter and re-publishing an
    // unchanged prompt every 15 s poll would be pure churn.
    if (st.notification != last_notify_code_) {
        last_notify_code_ = st.notification;
        if (notify_cb_) {
            notify_cb_(st.notification);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void BedjetBLE::post_notification(const uint8_t *data, uint16_t len)
{
    // BedJet V3 pushes the whole state block in one notification. Anything that
    // is not exactly BEDJET_NOTIFICATION_LEN bytes is discarded rather than
    // reassembled.
    if (data == nullptr || len != BEDJET_NOTIFICATION_LEN) {
        ESP_LOGW(TAG, "Discarding %u byte status notification (expected %u)",
                 len, static_cast<unsigned>(BEDJET_NOTIFICATION_LEN));
        return;
    }

    // This runs on the NimBLE host *task*, not an ISR: use the task critical
    // section (the same notify_mux_ the reader takes) rather than the
    // _FROM_ISR variants, which are only valid in interrupt context.
    taskENTER_CRITICAL(&notify_mux_);
    last_notify_ticks_ = xTaskGetTickCount();
    taskEXIT_CRITICAL(&notify_mux_);

    BedjetNotification n{};
    std::memcpy(&n, data, sizeof(n));

    if (status_cb_) {
        status_cb_(n);
    }
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

void BedjetBLE::run()
{
    ESP_LOGI(TAG, "BLE task started");

    const bool have_saved = load_mac_from_nvs();
    if (!have_saved) {
        ESP_LOGI(TAG, "No saved BedJet address yet, running discovery");
    } else {
        // Fast path: a known address connects in ~2-3 s. Discovery is only
        // the fallback when the saved device stops answering.
        has_device_.store(true, std::memory_order_release);
    }

    uint32_t last_poll = xTaskGetTickCount();

    for (;;) {
        if (!has_device_.load(std::memory_order_acquire)) {
            // Active scan whenever there is no trusted saved address to match
            // against (first-ever discovery, or the saved MAC was discarded);
            // passive only while a saved address is still trusted.
            if (!discover_device(!have_saved_addr_)) {
                ESP_LOGW(TAG, "No BedJet found, retrying in %" PRIu32 " ms", backoff_ms_);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms_));
                backoff_ms_ = std::min(backoff_ms_ * 2, BACKOFF_MAX_MS);
                continue;
            }
            has_device_.store(true, std::memory_order_release);
            saved_failures_ = 0;
        }

        if (link_once()) {
            saved_failures_ = 0;
            last_poll = xTaskGetTickCount();
            taskENTER_CRITICAL(&notify_mux_);
            last_notify_ticks_ = last_poll;
            taskEXIT_CRITICAL(&notify_mux_);
            int poll_failures = 0;

            while (is_connected()) {
                BedjetPacket pkt{};
                if (xQueueReceive(cmd_queue_, &pkt, pdMS_TO_TICKS(500)) == pdTRUE) {
                    write_packet(pkt);
                }

                const TickType_t now = xTaskGetTickCount();
                if ((now - last_poll) >= pdMS_TO_TICKS(STATUS_POLL_MS)) {
                    last_poll = now;
                    if (read_device_status()) {
                        // A completed GATT round-trip proves the link is alive
                        // even when the BedJet is in standby and sends no
                        // notifications; refresh liveness so the watchdog does
                        // not recycle a healthy idle link.
                        poll_failures = 0;
                        taskENTER_CRITICAL(&notify_mux_);
                        last_notify_ticks_ = xTaskGetTickCount();
                        taskEXIT_CRITICAL(&notify_mux_);
                    } else if (++poll_failures >= STATUS_POLL_FAILURES) {
                        ESP_LOGW(TAG, "Status poll failed %d times, recycling link",
                                 poll_failures);
                        break;
                    }
                }

                // Watchdog: trip when neither a status notification nor a
                // successful flags poll has landed for NOTIFY_WATCHDOG_MS, so a
                // silently wedged link is torn down and re-subscribed while a
                // healthy idle link survives.
                uint32_t last_notify;
                taskENTER_CRITICAL(&notify_mux_);
                last_notify = last_notify_ticks_;
                taskEXIT_CRITICAL(&notify_mux_);
                if ((now - last_notify) >= pdMS_TO_TICKS(NOTIFY_WATCHDOG_MS)) {
                    ESP_LOGW(TAG, "No status activity for %" PRIu32 " ms, recycling link",
                             NOTIFY_WATCHDOG_MS);
                    break;
                }
            }

            ESP_LOGW(TAG, "Link down, reconnecting");
        } else {
            ++saved_failures_;
            ESP_LOGW(TAG, "Link attempt failed (%d/%d), retrying in %" PRIu32 " ms",
                     saved_failures_, MAX_SAVED_ADDR_FAILURES, backoff_ms_);
            if (saved_failures_ >= MAX_SAVED_ADDR_FAILURES) {
                // The saved device may be gone (replaced unit, factory reset
                // with new MAC): discard it and rediscover rather than
                // hammering a dead address forever.
                ESP_LOGW(TAG, "Saved BedJet unreachable, rediscovering");
                has_device_.store(false, std::memory_order_release);
                // Drop address trust too: the next discovery must be an active
                // scan, or a unit whose name only appears in the scan response
                // can never be re-found.
                have_saved_addr_ = false;
                saved_failures_ = 0;
            }
        }

        teardown_link();
        vTaskDelay(pdMS_TO_TICKS(backoff_ms_));
        backoff_ms_ = std::min(backoff_ms_ * 2, BACKOFF_MAX_MS);
    }
}

} // namespace bedjet