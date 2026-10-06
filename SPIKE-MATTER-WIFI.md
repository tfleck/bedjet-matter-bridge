# SPIKE: Matter over Wi-Fi (alongside the Thread firmware)

Status: **scaffold — build-verified, runtime not tested.** All five board/transport
combinations build (sizes below) but none has been run on hardware. Branch
`spike/matter-wifi`, based on `main`. The Thread firmware is unchanged (default
transport).

## Objective

Add a Wi-Fi transport for the same Matter bridge, selectable per build, so the tree can
produce:

- **ESP32-C6** — Thread *or* Wi-Fi
- **ESP32-S3 / ESP32-C3 / ESP32** — Wi-Fi only (no 802.15.4)

and, for the Wi-Fi build, OTA the firmware directly from GitHub (if it fits the flash).

## Transport switch (build-time)

The transport is a CMake option that layers a per-transport defaults file on top of the
common one:

```
CMakeLists.txt
  set(MATTER_TRANSPORT "thread" CACHE STRING "thread or wifi")
  set(SDKCONFIG_DEFAULTS sdkconfig.defaults sdkconfig.defaults.${MATTER_TRANSPORT})
```

- `sdkconfig.defaults` — transport-agnostic (system, flash/partitions, BLE, crypto,
  Matter data model). **No longer pins `CONFIG_IDF_TARGET`** so one tree can build for
  all boards; run `idf.py set-target <board>` first.
- `sdkconfig.defaults.thread` — 802.15.4 + OpenThread + `CONFIG_ENABLE_WIFI_STATION=n`
  (the current defaults, unchanged).
- `sdkconfig.defaults.wifi` — `CONFIG_ENABLE_WIFI_STATION=y`, Thread/802.15.4 off,
  SoftAP off, IPv6/mDNS on, optional `CONFIG_APP_WIFI_OTA`.

Build:

```bash
idf.py -DMATTER_TRANSPORT=wifi set-target esp32c6
idf.py -DMATTER_TRANSPORT=wifi build
```

Thread stays the default, so `idf.py set-target esp32c6 && idf.py build` is unchanged.

## Code changes

Thread-specific code in `main.cpp` is now guarded by `CHIP_DEVICE_CONFIG_ENABLE_THREAD`:
the OpenThread includes, and the "default Thread dataset" log (the active-dataset log
was already guarded). The `kThreadConnectivityChange` handler is harmless on Wi-Fi
(never fires). `bedjet_matter.cpp` is transport-agnostic.

ESP-Matter handles the rest from Kconfig: with `CONFIG_ENABLE_WIFI_STATION=y` it wires
the Wi-Fi NetworkCommissioning cluster, and the commissioner delivers Wi-Fi credentials
over CHIPoBLE (PASE) — the same commissioning path the Thread build uses.

## Board matrix

| Board | Thread | Wi-Fi | Notes |
|---|---|---|---|
| ESP32-C6 | ✅ current | ✅ | combo radio; Wi-Fi + BLE coexist; no 802.15.4 in Wi-Fi build |
| ESP32-S3 | — | ✅ | |
| ESP32-C3 | — | ✅ | |
| ESP32 (original) | — | ⚠️ | CHIP ESP32 port supports it but is the tightest for flash/RAM; may need trims |

`.github/workflows/spike-build.yml` builds all five combinations to prove the switch.

## OTA directly from GitHub (Wi-Fi only)

Design (spike scaffold in `main/wifi_ota.cpp`, `CONFIG_APP_WIFI_OTA`, off by default):

1. After the node is provisioned onto Wi-Fi, query the GitHub releases API
   (`/repos/tfleck/bedjet-matter-bridge/releases/latest`) and compare the tag to the
   running `esp_app_get_description()->version`.
2. If newer, `esp_https_ota()` the **app-only** image to the inactive OTA slot. The
   certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`) validates the TLS chain;
   `otadata` handles the slot switch and rollback.

Requirements / caveats:

- **The image must be app-only.** `esp_https_ota` cannot consume the combined or
  partition-part images, so the release must expose `firmware_<target>_app.bin` (an
  ESP app image with `esp_app_desc`). The release workflow currently ships the combined
  image and the parts; add a clean app-only asset for OTA.
- **Flash budget — measured, fits.** All transports/boards build and fit the shared
  4 MB layout (`0x1F0000` = 2,031,616 B per slot), so OTA is kept rather than nixed:

  | Board / transport | app size (bytes) | free in slot |
  |---|---|---|
  | esp32c6 / wifi | 1,799,712 | 231,904 (11.4%) |
  | esp32c6 / thread | 1,629,552 | 402,064 (19.8%) |
  | esp32c3 / wifi | 1,635,024 | 396,592 (19.5%) |
  | esp32 / wifi | 1,509,552 | 522,064 (25.7%) |
  | esp32s3 / wifi | 1,508,752 | 522,864 (25.7%) |

  C6 Wi-Fi is the largest; `esp_https_ota` adds well under the remaining 11%. Still,
  if a future change tightens it, the fallback is to drop OTA and update every board
  through the flasher, or move to 8 MB (`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y` + `0x300000`
  slots).
- `releases/latest/download/<asset>` is a stable redirect to the newest asset and
  works without the JSON API, but it cannot cheaply tell you the version, so the API
  check is still wanted to avoid reflashing the same build.

## Pipeline plan (not done here)

The release matrix gains a transport dimension; asset names become
`firmware_<target>_<transport>_*` (e.g. `firmware_esp32c6_wifi_combined.bin`,
`firmware_esp32c6_wifi_app.bin`). The web flasher already keys off release assets and
would show a per-transport entry; the Wi-Fi OTA path uses the `_app.bin` asset. The
spike keeps this out of `build-release.yml` so the Thread release flow is untouched;
`spike-build.yml` only builds.

## Open questions / risks

- ~~Does the Wi-Fi Matter build fit a 4 MB slot?~~ **Resolved: yes** (table above); OTA
  kept.
- ESP32 (original) fits (25.7% free) but is the oldest/slowest target; runtime RAM
  headroom for Matter + Wi-Fi + a BLE central is still untested.
- Wi-Fi provisioning UX and reconnect behaviour vs the Thread MTD model.
- Coexistence of Wi-Fi + BLE central (single radio) under the BedJet link.
- OTA trigger policy (boot check, Matter OTA Requestor, or a manual action) and the
  app-only release asset.

## How to validate

- Local: `idf.py -DMATTER_TRANSPORT=wifi set-target esp32c6 && idf.py build` (requires
  the matching IDF; not run here).
- CI: push this branch / run **Spike build** — five jobs, one per board+transport.

## Rollback

Delete the branch. `main` is unchanged and still builds Thread-only on the C6.
