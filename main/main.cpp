#include <stdio.h>

#include <inttypes.h>

#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app/server/Server.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <platform/internal/BLEManager.h>
#include <esp_openthread_types.h>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <openthread/link.h>
#include <openthread/thread.h>

#include "bedjet_ble.h"
#include "bedjet_matter.h"

static const char *TAG = "main";

static bedjet::BedjetBLE   g_ble;
static bedjet::BedjetMatter g_matter;

static void bedjet_status_cb(const bedjet::BedjetNotification &n);
static void bedjet_conn_cb(bool connected);
static void bedjet_notify_cb(uint8_t code);

// True once the BedJet central is allowed to own the NimBLE stack. Boot order
// is Matter-first: CHIPoBLE owns NimBLE during PASE commissioning. BedJet
// must not init before that point - both stacks call nimble_port_init() +
// nimble_port_freertos_init() on the single C6 BLE controller, and the second
// init fails with "invalid controller state" and disables CHIPoBLE (5b00103).
//
// After commissioning completes we shut Matter BLE down WITHOUT memory
// release (CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING must stay off: the on path
// calls esp_bt_mem_release() which permanently frees the BLE BSS/data to the
// heap, so a later nimble_port_init() can never succeed). Matter's DriveBLEState
// then deinits nimble_port and the BedJet central can do a fresh init. Note
// kBLEDeinitialized only fires on the mem-release path, so the handover below
// retries from the System layer timer rather than waiting for that event.
static bool g_ble_handover_done = false;
static bool g_ble_handover_scheduled = false;

static void start_bedjet_ble()
{
    if (g_ble_handover_done) {
        return;
    }
    // Abort the take-over if Matter still owns the host: starting our own
    // nimble_port + host task underneath Matter breaks the commissioner link
    // (the Apple "unable to connect" hang). Both stacks spawn their host task
    // as "nimble_host" (nimble_port_freertos.c), so its presence means the
    // stack is still owned. Once we own it ourselves (!host_ready() is false),
    // a retry must proceed to finish starting rather than bail on the shared
    // task name. (Do NOT call ble_hs_is_enabled() here: after
    // nimble_port_deinit() the host mutex is gone and it load-faults.)
    if (!g_ble.host_ready() && xTaskGetHandle("nimble_host") != nullptr) {
        return;
    }
    g_ble.on_status(bedjet_status_cb);
    g_ble.on_conn_state(bedjet_conn_cb);
    g_ble.on_notify(bedjet_notify_cb);
    if (!g_ble.init()) {
        ESP_LOGE(TAG, "BedJet BLE initialisation failed - will retry after Matter BLE shutdown settles");
        return;
    }
    g_ble_handover_done = true;
}

namespace {

// Number of 500 ms retries for the BedJet take-over after Matter BLE
// shutdown, before giving up until the next reboot.
constexpr int kHandoverRetries = 20;

// Grace period after boot before the commissioned-boot BedJet handover takes
// the single radio. Starting the BLE scan while Thread/SRP is still registering
// showed up as ChannelAccessFailure + a multi-second SRP timeout in boot logs.
// Fresh commissioning (kCommissioningComplete) is unaffected and hands over
// immediately. 2 s is enough: SRP advertises and the server is ready by ~1.8 s
// in measured boots, while 5 s only delayed the BedJet link. Tune by
// measurement.
constexpr uint32_t kBootHandoverSettleMs = 2000;

void handover_retry(chip::System::Layer * layer, void * app_state);

void handover_shutdown(intptr_t)
{
    // Called on the CHIP task via ScheduleWork: safe to touch the BLEManager
    // state machine (Shutdown just changes flags and schedules DriveBLEState).
    // The blocking BedJet init is deliberately NOT done here - see
    // hand_over_ble_to_bedjet() - only the first retry is kicked off on the
    // System layer.
    chip::DeviceLayer::Internal::BLEMgr().Shutdown();
    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(500), handover_retry,
                                                reinterpret_cast<void *>(0));
}

void handover_retry(chip::System::Layer * layer, void * app_state)
{
    const intptr_t attempt = reinterpret_cast<intptr_t>(app_state);
    if (!g_ble_handover_done) {
        ESP_LOGI(TAG, "BedJet BLE handover attempt %" PRIdPTR, attempt);
        start_bedjet_ble();
    }
    if (!g_ble_handover_done && attempt < kHandoverRetries) {
        constexpr uint32_t kRetryMs = 500;
        layer->StartTimer(chip::System::Clock::Milliseconds32(kRetryMs), handover_retry,
                          reinterpret_cast<void *>(attempt + 1));
    } else if (!g_ble_handover_done) {
        ESP_LOGE(TAG, "BedJet BLE handover failed after retries - reboot to retry");
    }
}

} // namespace

static void hand_over_ble_to_bedjet()
{
    // Ask Matter to tear down CHIPoBLE (stops adv, deinits nimble_port, but
    // keeps the BLE BSS/data so we can re-init), then retry the BedJet init
    // from the System layer timer until nimble_port is free. The retries must
    // NOT run on the CHIP task: NimBLEDevice::init() blocks on the host sync
    // semaphore, which only the new nimble_host task can give - running that
    // wait on the CHIP task deadlocks Matter (CASE/IM stop, Apple errors out).
    // kBLEDeinitialized only fires on the mem-release path (deliberately
    // avoided), so it cannot be the handover signal.
    if (g_ble_handover_scheduled) {
        return;
    }
    g_ble_handover_scheduled = true;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(handover_shutdown, 0) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to schedule Matter BLE shutdown");
        g_ble_handover_scheduled = false;
    } else {
        ESP_LOGI(TAG, "Requested Matter BLE shutdown - BedJet handover pending");
    }
}

static void handover_after_settle(chip::System::Layer *, void *)
{
    hand_over_ble_to_bedjet();
}

// Runs on the CHIP task (via ScheduleWork) so the System-layer timer is armed
// from the same context that already owns it in handover_shutdown().
static void handover_settle_work(intptr_t)
{
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(kBootHandoverSettleMs),
        handover_after_settle, reinterpret_cast<void *>(0));
}

static void matter_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    if (event == nullptr) {
        return;
    }

    namespace DeviceEventType = chip::DeviceLayer::DeviceEventType;
    constexpr auto established = chip::DeviceLayer::ConnectivityChange::kConnectivity_Established;

    switch (event->Type) {
    case DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGW(TAG, "Commissioning window is open - this device can be paired");
        g_matter.print_pairing_info();
        break;

    case DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "A Matter commissioner connected");
        break;

    case DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Matter commissioner disconnected");
        break;

    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete - this device is paired");
        // Hand NimBLE over to the BedJet central. This only schedules the
        // System-layer handover (async): the direct start below is skipped
        // because NimBLEDevice::init() blocks on host sync and must never run
        // on the Matter event callback (it would stall CASE/IM whose timers
        // share the CHIP task - the Apple "unable to connect" hang). The
        // System-layer timer fires on its own task instead.
        hand_over_ble_to_bedjet();
        break;

    case DeviceEventType::kThreadConnectivityChange: {
        const bool attached =
            event->ThreadConnectivityChange.Result == established;
        ESP_LOGI(TAG, "Thread %s", attached ? "attached" : "detached");
        // Track attach timing so a flapping mesh (attach/detach cycling) is
        // distinguishable from a clean attach in the log.
        static uint32_t last_attach_ms = 0;
        static int detach_count = 0;
        const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (attached) {
            if (last_attach_ms != 0 && detach_count > 0) {
                ESP_LOGW(TAG, "Thread re-attached after %lu ms (%d detaches since last attach)",
                         (unsigned long)(now_ms - last_attach_ms), detach_count);
            }
            last_attach_ms = now_ms;
            detach_count = 0;
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
            // Report the *active* operational dataset, not the compile-time
            // default printed at boot: on a commissioned node these differ.
            if (esp_openthread_lock_acquire(portMAX_DELAY)) {
                otInstance *ot = esp_openthread_get_instance();
                if (ot != nullptr) {
                    ESP_LOGI(TAG, "Active Thread network: name=\"%s\" channel=%u panid=0x%04x",
                             otThreadGetNetworkName(ot), (unsigned)otLinkGetChannel(ot),
                             (unsigned)otLinkGetPanId(ot));
                }
                esp_openthread_lock_release();
            }
#endif
        } else {
            ++detach_count;
            if (last_attach_ms != 0) {
                ESP_LOGW(TAG, "Thread detached %lu ms after last attach (count %d)",
                         (unsigned long)(now_ms - last_attach_ms), detach_count);
            }
        }
        break;
    }

    case DeviceEventType::kServiceConnectivityChange:
        ESP_LOGI(TAG, "Matter service reachable over Thread: %s",
                 event->ServiceConnectivityChange.Overall.Result == established ? "yes" : "no");
        break;

    case DeviceEventType::kWiFiConnectivityChange:
        ESP_LOGI(TAG, "WiFi %s",
                 event->WiFiConnectivityChange.Result == established ? "connected" : "disconnected");
        break;

    default:
        break;
    }
}

static void bedjet_status_cb(const bedjet::BedjetNotification &n)
{
    // Runs on the NimBLE host task. This must stay non-blocking.
    g_matter.post_status(n);
}

static void bedjet_conn_cb(bool connected)
{
    // Runs on the BLE task. Non-blocking: enqueue the link state so the Matter
    // bridge task owns the Reachable attribute write and updates it once the
    // BedJet link changes.
    g_matter.post_conn_state(connected);
    if (connected) {
        ESP_LOGI(TAG, "BedJet link is up; the bridge is live");
    } else {
        ESP_LOGW(TAG, "BedJet link is down; Matter attributes will go stale");
    }
}

static void bedjet_notify_cb(uint8_t code)
{
    // Runs on the BLE task (status poll). Non-blocking: enqueue the prompt code
    // for the Matter bridge task, which owns the filter attribute write.
    g_matter.post_notify_code(code);
}

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH:return "power glitch";
    case ESP_RST_CPU_LOCKUP:return "CPU lockup";
    default:                return "unknown";
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "bedjet-matter-bridge starting (esp-idf %d.%d.%d)",
             ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Boot reason: %s (%d)", reset_reason_str(reason), (int)reason);
    // Compile-time default dataset: only used until a commissioner provisions
    // a real dataset. The *active* dataset is logged on Thread attach below.
    ESP_LOGI(TAG, "Default Thread dataset (pre-commissioning): name=\"%s\" channel=%d panid=0x%04x",
             CONFIG_OPENTHREAD_NETWORK_NAME,
             CONFIG_OPENTHREAD_NETWORK_CHANNEL,
             CONFIG_OPENTHREAD_NETWORK_PANID);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS and retrying");
        ret = nvs_flash_erase();
        if (ret == ESP_OK) {
            ret = nvs_flash_init();
        }
    }
    if (ret != ESP_OK) {
        // Do not reboot the device here: a NVS problem should not brick a device
        // that is otherwise reachable over Thread for a factory reset.
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ESP-IDF's CHIP port routes CHIP categories straight through ESP_LOG
    // (Error->ERROR, Progress->INFO, Detail->DEBUG), so the CHIP runtime log
    // filter does not silence them: chip[EM]/chip[SC]/chip[DIS] flood the log.
    // Quiet the CHIP module tags directly; the bridge's own tags stay at INFO.
    esp_log_level_set("esp_matter_attribute", ESP_LOG_WARN);
    esp_log_level_set("chip[-]", ESP_LOG_WARN);
    esp_log_level_set("chip[BLE]", ESP_LOG_WARN);
    esp_log_level_set("chip[DL]", ESP_LOG_WARN);
    esp_log_level_set("chip[DMG]", ESP_LOG_WARN);
    esp_log_level_set("chip[DIS]", ESP_LOG_WARN);
    esp_log_level_set("chip[EM]", ESP_LOG_WARN);
    esp_log_level_set("chip[FP]", ESP_LOG_WARN);
    esp_log_level_set("chip[FS]", ESP_LOG_WARN);
    esp_log_level_set("chip[IM]", ESP_LOG_WARN);
    esp_log_level_set("chip[IN]", ESP_LOG_WARN);
    esp_log_level_set("chip[SC]", ESP_LOG_WARN);
    esp_log_level_set("chip[SVR]", ESP_LOG_WARN);
    esp_log_level_set("chip[TS]", ESP_LOG_WARN);
    esp_log_level_set("chip[ZCL]", ESP_LOG_WARN);

    // 1. Build the Matter endpoint structure. This must exist before start()
    //    because attribute writes route through the node. The BLE status
    //    callback posts into the Matter bridge queue, so it is safe to register
    //    before either stack owns the radio.
    if (!g_matter.init(&g_ble)) {
        ESP_LOGE(TAG, "Matter endpoint setup failed");
        return;
    }

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t ot_platform_config = {};
    ot_platform_config.radio_config.radio_mode = RADIO_MODE_NATIVE;
    ot_platform_config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
    ot_platform_config.port_config.storage_partition_name = "nvs";
    // 32-deep queues survive Apple's post-reboot subscription burst (all
    // attributes re-subscribed at once) without dropping MLE; costs ~4 KB.
    ot_platform_config.port_config.netif_queue_size = 32;
    ot_platform_config.port_config.task_queue_size = 32;
    ESP_ERROR_CHECK(set_openthread_platform_config(&ot_platform_config));
#endif

    // 2. Start the Matter stack FIRST so its CHIPoBLE layer owns NimBLE during
    //    commissioning. The BedJet central must not init yet (see above).
    if (esp_matter::start(matter_event_cb) != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed");
        return;
    }

    // Attribute reports are only safe once the CHIP stack is up.
    g_matter.mark_matter_started();

    // (CHIP logs are quieted via the esp_log_level_set calls above. The CHIP
    // runtime filter, chip::Logging::SetLogFilter(), is a no-op on ESP-IDF.)

    // 3. If a fabric already exists, this boot never starts CHIPoBLE, so the
    //    handover already applies. Matter may still be holding nimble_port
    //    while its startup completes, so go through the retry path rather
    //    than a single shot: if the host task is still up the first attempt
    //    defers and the System-layer timer retries until it is free.
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() != 0) {
        // Let Thread/SRP settle before taking the single radio for BLE. The
        // retry path below still applies once the timer fires.
        ESP_LOGI(TAG, "Already commissioned - settling %" PRIu32 " ms for Thread/SRP before BedJet handover",
                 kBootHandoverSettleMs);
        if (chip::DeviceLayer::PlatformMgr().ScheduleWork(handover_settle_work, 0) != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to schedule BedJet handover settle");
        }
    } else if (chip::DeviceLayer::Internal::BLEMgr().IsAdvertisingEnabled()) {
        // CHIPoBLE is up and awaiting the commissioner: BedJet stays off the
        // radio until the handover so PASE + Thread dataset delivery own BLE.
        ESP_LOGI(TAG, "Uncommissioned - BedJet BLE deferred until commissioning completes");
    } else {
        // CHIPoBLE never came up (e.g. commissioner already gone, or BLE init
        // failed). Do not deadlock the bridge: start the BedJet central so the
        // device side still works and the next window can be opened manually.
        ESP_LOGW(TAG, "Uncommissioned but CHIPoBLE is down - starting BedJet BLE anyway");
        start_bedjet_ble();
    }

    ESP_LOGI(TAG, "Boot complete. Watch the log for commissioning and Thread state.");
}