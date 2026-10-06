#include "wifi_ota.h"

#ifdef CONFIG_APP_WIFI_OTA

#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Direct-from-GitHub OTA for the Wi-Fi transport (spike).
//
// esp_https_ota writes the image to the inactive OTA slot and marks it to boot,
// so `otadata` handles the switch and a bad image rolls back. It requires an
// *app-only* image (an ESP app image carrying esp_app_desc) - the combined /
// partition-part images are NOT usable here, so the release must expose
// firmware_<target>_app.bin. The target is hardcoded for the initial C6 bring-up.
//
// This is a single-shot check; a production version should query the GitHub
// releases API and only download when the tag is newer than the running
// ESP_APP_DESC.version (see SPIKE-MATTER-WIFI.md).
#define OTA_APP_URL \
    "https://github.com/tfleck/bedjet-matter-bridge/releases/latest/download/firmware_esp32c6_app.bin"

static const char *TAG = "wifi_ota";

static void ota_task(void *)
{
    ESP_LOGI(TAG, "Checking for a firmware update: %s", OTA_APP_URL);

    esp_http_client_config_t http = {};
    http.url = OTA_APP_URL;
    // The certificate bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y) validates the
    // github.com / objects.githubusercontent.com TLS chain.
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 20000;
    http.keep_alive_enable = true;

    esp_https_ota_config_t ota = {};
    ota.http_config = &http;

    esp_err_t err = esp_https_ota(&ota);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA applied, rebooting");
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    vTaskDelete(nullptr);
}

void bedjet::wifi_ota_start()
{
    xTaskCreate(ota_task, "wifi_ota", 8192, nullptr, 3, nullptr);
}

#else

void bedjet::wifi_ota_start() {}

#endif // CONFIG_APP_WIFI_OTA
