#pragma once

namespace bedjet {

// Starts the Wi-Fi/GitHub OTA checker. No-op unless CONFIG_APP_WIFI_OTA is set.
// See SPIKE-MATTER-WIFI.md.
void wifi_ota_start();

} // namespace bedjet
