# Agent notes

Repo-specific instructions for AI agents working in this checkout.
The README covers what the firmware does; this file covers how to work on it.

## ESP-IDF environment (Windows)

- Toolchain lives at `C:\esp-idf\v5.5.5\esp-idf`. Every shell command that
  touches the build must source it first:
  `C:\esp-idf\v5.5.5\esp-idf\export.ps1 | Out-Null`
- Target is ESP32-C6 (`CONFIG_IDF_TARGET="esp32c6"` in `sdkconfig.defaults`).
  `sdkconfig` itself is gitignored; `sdkconfig.defaults` is authoritative.
- Device is usually on `COM3`. Flash with `idf.py -p COM3 flash`.
  If the port reports "Access is denied", a serial monitor holds it; close
  the monitor (or any previous `idf.py monitor`) and retry. Never force it.

## Faster builds

- **ccache is already on** (`ccache will be used for faster recompilation`
  appears in cmake output). Do not disable it.
- **Never `fullclean` to fix a stale-ninja error.** If `build.ninja` goes
  missing (`ninja: error: loading 'build.ninja'`) after deleting it by hand,
  run `idf.py reconfigure`, not a full rebuild. `fullclean` costs ~30+ min
  (Matter is ~2200 objects); `reconfigure` costs seconds.
- **Full Matter builds exceed the 10-minute tool timeout.** Run
  `idf.py build` via the `background_process` tool (`start`), then poll with
  `logs`/`status`. Do not chain blocking builds in one command.
- **Resume, don't restart.** An interrupted build continues where it left
  off; just run `idf.py build` again. Object count
  (`(Get-ChildItem build\esp-idf -Recurse -Filter "*.obj" | Measure).Count`
  vs ~2188) shows progress.
- **Incremental edits are cheap.** Touching `main/*` rebuilds a handful of
  objects plus relink (~1-2 min with ccache warm). Only `sdkconfig.defaults`,
  `CMakeLists.txt`, or managed-component changes trigger wide rebuilds.
- **Read the VS Code Problems pane before building.** clangd flags type
  errors (e.g. wrong `discover_device()` signature) in seconds; a build
  takes minutes. Fix lint first, build once.

## Architecture (non-obvious)

- Single C6 radio runs **Thread + BLE central simultaneously**. BLE and
  Matter fight over one NimBLE stack: boot order is Matter-first, BedJet
  central starts only after the `kCommissioningComplete` handover
  (`main.cpp`). Starting `g_ble.init()` earlier double-inits
  `nimble_port` and kills CHIPoBLE (`5b00103`).
- `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING` must stay **off**: the on-path calls
  `esp_bt_mem_release()`, permanently freeing BLE memory so the BedJet side
  can never re-init.
- Rendezvous flag in `print_pairing_info()` is `kBLE` deliberately. It is the
  *commissioning* transport (PASE + Thread dataset over CHIPoBLE), not the
  operational network. `kThread` makes Apple try Thread DNS-SD against the
  unprovisioned `OpenThread-ESP` network and hang.
- The node is an **MTD, not FTD** (`sdkconfig.defaults`). Do not flip back to
  FTD unless the BedJet needs to route for a distant Apple TV.
- `bedjet_matter.cpp` publishes BedJet state via a bridge task + publish-guard
  (`publishing_`): device-originated `attribute::report()` on writable attrs
  would otherwise re-enter PRE_UPDATE and loop back into a BedJet command.

## BLE protocol essentials

- BedJet V3 GATT (`bedjet_protocol.h`): service `...BED0-...`,
  command char `...2004` (write/read), name char `...2000`, status char
  `...2001` (20-byte notify), flags char `...2002` (11-byte poll).
  Commands are V1-framed (`0x01 len cmd CRC8`), responses arrive as notifies.
- Wire resolution is **half-degree steps** (`temp_step = C * 2`) and 20 fan
  steps (5%..100%). Convert with integer math (`step * 50` centi-degrees);
  float round-trips show 1 F off in HomeKit vs the radio remote.
- GATT table is static, but the esp-nimble-cpp client's service objects are
  deleted on disconnect: always `connect(deleteAttributes=true)` and
  re-resolve handles per link. Caching raw handles across links is a
  use-after-free panic.

## Known-benign log noise (do not "fix")

- `Failed to process UDP - Duplicated` (MLE pairs): normal with Apple TV
  routers nearby.
- `chip[DIS] ... : 3` / `Operational advertising failed: 3` at boot: advertiser
  `RemoveServices` before `Init`; the retry ~2 s later publishes fine.
- `unknown session LSID` + `subscription-resumption CASE fail: 32` after
  reboot: stale Apple session + 45 s offline-peer lookup timeout. Self-heals.
- `GetClock_RealTimeMS: 6c`: C6 has no RTC; Last-Known-Good-Time fallback.
- `DefaultAclStorage: 0 entries` on first boot is expected (uncommissioned).
