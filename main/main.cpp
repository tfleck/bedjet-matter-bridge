#include <cstdio>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "protocol_examples_common.h"
#include "esp_matter.h"
#include "esp_timer.h"
#include "bedjet_ble.h"
#include "bedjet_matter.h"
#include "bedjet_ota.h"

static const char* TAG = "main";

#define GITHUB_OWNER   "tfleck"
#define GITHUB_REPO    "bedjet-matter-bridge"
#define APP_VERSION    "v1.0.0"

static const uint8_t BEDJET_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

static bedjet::BedjetBLE g_ble;
static bedjet::BedjetMatter g_matter_inst;
static bedjet::BedjetOTA g_ota(GITHUB_OWNER, GITHUB_REPO);
bedjet::BedjetMatter* g_matter = nullptr;

static esp_timer_handle_t ota_check_timer;

static void ota_check_timer_cb(void* arg) {
    (void)arg;
    bedjet::GitHubRelease release;
    if (g_ota.check_for_release(release)) {
        ESP_LOGI(TAG, "New release found! Starting OTA...");
        g_ota.trigger_ota(release.bin_url.c_str());
    }
}

static void setup_ota_timer() {
    esp_timer_create_args_t args = {
        .callback = ota_check_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_check",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &ota_check_timer);
    esp_timer_start_periodic(ota_check_timer, 3600 * 1000000ULL);
}

extern void app_connect();

void app_main(void) {
    ESP_LOGI(TAG, "==================================");
    ESP_LOGI(TAG, "   BedJet Matter Bridge %s", APP_VERSION);
    ESP_LOGI(TAG, "==================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_wifi_init());
    app_connect();
    ESP_LOGI(TAG, "WiFi connected");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    if (!g_ble.init()) {
        ESP_LOGE(TAG, "BLE init failed");
        return;
    }

    g_ble.on_status([](const bedjet::BedjetStatusPacket& pkt) {
        if (g_matter) g_matter->update_status(pkt);
    });

    g_ble.on_conn_state([](bool connected) {
        ESP_LOGI(TAG, "BedJet %s", connected ? "connected" : "disconnected");
    });

    g_matter = &g_matter_inst;
    if (!g_matter_inst.init(&g_ble)) {
        ESP_LOGE(TAG, "Matter init failed");
        return;
    }

    g_ota.set_current_version(APP_VERSION);
    setup_ota_timer();

    vTaskDelay(pdMS_TO_TICKS(5000));
    g_ble.connect(BEDJET_MAC);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (g_ble.is_connected()) {
            g_ble.request_status();
        }
    }
}
