# BedJet Matter Bridge — QA Review & Remediation Plan

Review of the firmware in `main/` plus build, partition, CI, and web packaging.
Scope: reliability, performance, maintainability, and correctness. No code was
changed; every item below cites the current source.

Baseline verified from the working tree:

- `build/bedjet_matter_bridge.bin` = 1,969,376 B (0x1DEDE0).
- `partitions.csv` ota_0/ota_1 = 0x1F0000 each → **62,240 B (3.06%) free**. This is
  the hard budget gate: any fix that adds net code must first reclaim space.
- `report()`/`update()` in esp-matter take `lock::ScopedChipStackLock`
  (`esp_matter_attribute_utils.cpp:195`) and normalize "unchanged value" to
  `ESP_OK` (`:204-206`). The echo-loop guard is therefore sound and thread-safe.
- Notify callbacks fire on the NimBLE **host task**, not an ISR
  (`NimBLEClient.cpp:1204-1210`).

Severity: **H** = functional/reliability defect, **M** = latent defect or
resource/performance issue, **L** = cleanup/doc.

---

## Phase 0 — Budget gate (do first)

- **B1 (H) App partition is 3.06% free.** `partitions.csv:5`, README:234-260.
  Land the deletions in M1/M2 and comment-only changes first; then measure before
  adding anything. If net growth is unavoidable, resize to 8 MB
  (`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y` + 0x300000 slots) rather than shaving.
  **Acceptance:** `idf.py build` prints no `check_sizes.py` overflow / "nearly
  full" warning after the remediation.

### B1.1 Partition-table math on 4 MB (answer: the table cannot give app headroom)

Verified against `gen_esp32part.py`: app partition **offset** must be 0x10000
aligned (`ALIGNMENT[APP_TYPE]=0x10000`, `:106-115, :567-569`); app **size** only
needs 0x1000 alignment when secure boot is off (`:118-132`). Current 4 MB layout:

```
0x000000  bootloader (32 K)
0x008000  partition table (4 K)
0x009000  nvs 24 K → 0xF000
0x00F000  otadata 8 K → 0x11000
0x011000  phy_init 4 K → 0x12000
0x012000  UNUSED 56 K → 0x20000        <-- alignment gap
0x020000  ota_0 0x1F0000 → 0x210000
0x210000  ota_1 0x1F0000 → 0x400000
```

Because `ota_1`'s offset must be 64 K aligned and land on {0x20000, 0x210000,…},
the symmetric slot is pinned at **0x1F0000 (1,986,560 B)** on 4 MB, no matter how
the data partitions are rearranged. Repacking at most moves the 56 K gap; it does
not widen the app slot. Concretely:

- Put `ota_0` at 0x10000 and `otadata`/`coredump` at the end → same 0x1F0000.
- Shrink nvs to 16 K / drop `phy_init` → frees ~12 K of *data* only.
- The IDF reference two-OTA layout (`partitions_two_ota.csv`) uses 1 M slots and
  a `factory` partition — strictly worse here.

**Practical partition changes (not more app space, but worth doing):**

```csv
# Option A: keep the OTA geometry, harvest the 56 K gap for crash dumps.
nvs,      data, nvs,      0x9000,  0x6000,
otadata,  data, ota,      0xF000,  0x2000,
phy_init, data, phy,      0x11000, 0x1000,
coredump, data, coredump, 0x12000, 0xE000,
ota_0,    app,  ota_0,    0x20000, 0x1F0000,
ota_1,    app,  ota_1,    0x210000,0x1F0000,
```
(add `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`; summary/dump size configurable).

```csv
# Option C: 8 MB board — the only change that actually widens the slots.
nvs,      data, nvs,      0x9000,  0x6000,
otadata,  data, ota,      0xF000,  0x2000,
phy_init, data, phy,      0x11000, 0x1000,
ota_0,    app,  ota_0,    0x20000, 0x300000,
ota_1,    app,  ota_1,    0x320000,0x300000,
```
plus `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`.

### B1.2 Real headroom comes from the build, not the table

- **`CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` (-Og) is active** (`sdkconfig:641`);
  `CONFIG_COMPILER_OPTIMIZATION_SIZE` (-Os) is **not** set. Switching the whole
  project (including managed components) to `-Os` is the single largest reclaim;
  Matter/CHIP builds typically drop well over 100 KB this way. Low risk; measure
  after.
- Optional further shrink, with a diagnostics tradeoff:
  `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT/_DISABLE` and
  `CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=0` (`sdkconfig:645-652`).
- Audit compile-time features that Matter does not use at runtime, measure each:
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` (root CA bundle for TLS; Matter has no TLS),
  `CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT`, `CONFIG_LWIP_MAX_SOCKETS=16`,
  `CONFIG_USE_MINIMAL_MDNS`. Keep `OPENTHREAD_SRP_CLIENT` — Matter-on-Thread needs
  SRP to publish DNS-SD.

**Recommendation:** keep the 4 MB OTA table (it is already at its maximum), land
`-Os`, and spend the harvested space on the defect fixes plus a coredump partition.
Only move to 8 MB if the board provides it or the product needs more features.

---

## Phase 1 — Reliability defects

### R1 (M) BLE handover retry is not idempotent and can leak/duplicate
`main.cpp:46-69` retries `start_bedjet_ble()` up to 20× via `handover_retry`
(`main.cpp:91-105`). `BedjetBLE::init()` (`bedjet_ble.cpp:84-108`) unconditionally
`xQueueCreate()`s `cmd_queue_` and, on a later failure, can `xTaskCreate()` a
second `bedjet_ble` task or re-run `NimBLEDevice::init()`.

**Fix:** give `BedjetBLE` an `initialized_`/`host_ready_` state. Make `init()`
create the queue/task exactly once and return success/failure without re-init on
retry; keep the "wait for `nimble_host` to disappear" check in `main.cpp`
(`:59`) as the gate before the single real init.

### R2 (H) Recovery discovery is always passive after a saved MAC
`bedjet_ble.cpp:672` captures `const bool have_saved` once; `:688` calls
`discover_device(!have_saved)`, so once any MAC was loaded this is `false`
forever. When the saved address is discarded after failures (`:736-743`) the
subsequent scan is passive, and a BedJet whose name/service is only in the scan
response becomes undiscoverable — the bridge can get permanently stuck.

**Fix:** drive the scan mode from the *current* need, not a boot-time constant:
use `active_scan = !has_device_` (or a dedicated `rediscovering_` flag set when
the MAC is discarded). Optionally re-read NVS on each discovery cycle.

### R3 (M) Notify watchdog recycles a healthy, idle link
`NOTIFY_WATCHDOG_MS = 300000` (`bedjet_ble.h:92-98`) and the 60 s poll
(`bedjet_ble.cpp:711-715`) does **not** refresh `last_notify_ticks_`. A BedJet in
standby can be silent (the 20-byte frame only changes when runtime/state changes),
so the link is torn down ~every 5 min and re-subscribed, churning the shared
combo radio and interrupting control.

**Fix:** treat a successful `read_device_status()` (a real GATT round-trip) as
liveness and refresh `last_notify_ticks_` there; add a consecutive-failure counter
and only recycle after N failed polls or the extended silence window.

### R4 (M) ISR-only FreeRTOS APIs called from task context
`post_notification()` (`bedjet_ble.cpp:652-654`) uses
`taskENTER_CRITICAL_FROM_ISR()` / `xTaskGetTickCountFromISR()`, but the notify
callback runs on the NimBLE host **task**. It also uses a different mechanism than
the reader (`taskENTER_CRITICAL(&notify_mux_)`, `:701-703`/`:721-723`). This can
assert under the untested-function check and is at best accidental.

**Fix:** use `taskENTER_CRITICAL(&notify_mux_)` + `xTaskGetTickCount()` in both
writer and reader (single writer, single reader, one core), or an atomic tick.
Correct the `bedjet_ble.h:138-140` comment ("ISR context" is wrong).

### R5 (M) Global `publishing_` can swallow a genuine controller write
`PublishGuard` (`bedjet_matter.cpp:70-83`) is held for the whole `apply_status()`
(`:432`), spanning several `report()` calls. While it is set, a controller write
whose PRE_UPDATE callback runs between two of those reports is silently dropped as
"device-originated" (`:686`) — the cluster still stores the controller value, so
Home and the BedJet disagree until the next status.

**Fix:** scope the guard to each individual `report()` call (set/clear inside
`publish_*`) so the window is limited to the synchronous callback of that report,
not the entire status application.

### R6 (L) Rejected Matter commands are lost with no reconciliation
`do_system_mode`/`do_setpoint`/`do_percent_setting` (`bedjet_matter.cpp:600-668`)
return `false` when BLE is down or the queue is full, but HomeKit already accepted
the write. Nothing re-sends it. At minimum log at WARN and mark the attribute
stale; ideally retry once after reconnect (the device pushes state, so a status
reconciles direction but the user's command is not replayed).

### R7 (L) `do_setpoint` logs success even when the write failed
`bedjet_matter.cpp:626-628` logs the step before checking `set_temperature()`'s
return. Log only on `true`.

### R8 (L) Device-name callback path is dead
`found_cb_` is only invoked from `link_once()` (`bedjet_ble.cpp:458`), but
`on_device_found()` is never called (`main.cpp:62-63` registers only status/conn).
Either register a callback (e.g. to set NodeLabel per unit) or delete the callback
and the `Reporting in as` branch.

### R9 (L, also frees space) Unreachable "state held" branch
`apply_status`'s `!matter_started_` early return (`bedjet_matter.cpp:427-430`)
cannot fire: BLE start is always scheduled after `mark_matter_started()`
(`main.cpp:309` vs `:316-329`). The README's "state is held and published as soon
as Matter is up" is therefore not implemented. Delete the branch **or** store the
last `BedjetNotification` and replay it on `mark_matter_started()`; fix the README
line either way (README:402).

### R10 (L) Good saved MAC discarded after 3 transient failures
`bedjet_ble.cpp:733-743` resets `has_device_` and forces a 10 s scan after three
connect failures, so a merely powered-off BedJet triggers repeated scans. Consider
a larger threshold and/or keep connecting to the known address with backoff, only
rediscovering when the user clears the address.

### R11 (L) Unlocked `get_val` from `app_main`
`apply_identity()` (`bedjet_matter.cpp:296-301`) calls
`esp_matter::attribute::get_val` from the `app_main` task; unlike `report`/`update`,
`get_val` does **not** take the CHIP stack lock
(`esp_matter_data_model.cpp:877-918`). Wrap it in
`esp_matter::lock::ScopedChipStackLock` or run it via `PlatformMgr().ScheduleWork`.

---

## Phase 2 — Performance / radio behavior

- **P1 (M)** Radio churn from R3 + R10; fixing R3 and R10 removes the periodic
  re-subscribe/re-scan cycles.
- **P2 (L)** First-discovery scan is 100% duty cycle
  (`scan->setInterval(100); setWindow(100)`, `bedjet_ble.cpp:259-260`). If R2 makes
  active scans recur, use a duty-cycled active window (e.g. 50%) to protect
  Thread airtime.
- **P3 (L)** `read_device_status()` failures only log (`bedjet_ble.cpp:616-634`);
  add the failure counter from R3 so a wedged status characteristic recycles.
- **P4 (L)** The bridge loop comment claims "Interleave 1:1"
  (`bedjet_matter.cpp:326-328`) but drains up to 2 commands + 2 statuses per wake;
  correct the comment (behavior is fine).

---

## Phase 3 — Maintainability

- **M1 (M)** Delete the five unused wrappers
  `report_enum8/report_u8/report_nullable_u8/report_nullable_i16/report_i16`
  (`bedjet_matter.cpp:484-488, 507-511, 527-531, 553-557, 576-580`; declared
  `bedjet_matter.h:82-86`). They only forward to `publish_*`. Frees a little space.
- **M2 (M)** Remove or archive the stale `PLAN.md`. It describes an earlier
  firmware (placeholder MAC, fabricated 18-byte fragmentation parser, aggregator
  endpoint) and directly contradicts the current README and code. If kept, move to
  `docs/` with a "historical, superseded" header.
- **M3 (L)** Include hygiene: `bedjet_ble.cpp` uses `std::string`/`std::min`
  without `<string>`; `bedjet_matter.cpp` includes `<math.h>` that the integer-math
  temperature path no longer needs. Add/drop accordingly.
- **M4 (L)** Duplicated log: `log_mac("Discovered BedJet", ...)` followed by
  `ESP_LOGI("Discovered BedJet %s rssi", ...)` prints the address twice
  (`bedjet_ble.cpp:306-308`). Collapse to one.
- **M5 (L)** `cmd_write_response_` silently becomes `false` if the command
  characteristic lacks both write properties (`bedjet_ble.cpp:444-448`); then every
  write fails with only a WARN (`:600`). Log the "no usable write property" case
  explicitly at link time.
- **M6 (L)** State consistency: when the BedJet drops to standby, `FanMode`
  publishes Off but `PercentSetting` keeps its last value
  (`bedjet_matter.cpp:394-481`). Document this as intentional (commanded target vs
  measured state) or zero the setting on standby.

---

## Phase 4 — Build, packaging, docs

- **B2 (H)** Release workflow advertises and lists 6 chips but only ESP32-C6 is
  built (`.github/workflows/build-release.yml:243-270`, `files:` includes five
  bins that are never produced; the release body and `web/` list 6). This can fail
  the release job or ship dead links. Trim to ESP32-C6, or re-enable targets
  deliberately.
- **B3 (L)** `web/index.html:97-99` footer hrefs contain literal
  `{GITHUB_OWNER}`/`{GITHUB_REPO}` placeholders that are never substituted.
  Hardcode `tfleck/bedjet-matter-bridge` or template at deploy time.
- **B4 (L)** `web/` advertises non-C6 chips (`app.js:28-35`, `index.html:32-37`)
  and its ASCII-QR detector (`app.js:168-173`) can never match, because the
  firmware prints a QR *payload* string, not an ASCII QR (README:26-27). Trim the
  chip list and drop the QR-block UI (keep the manual code), or add real QR
  rendering.
- **B5 (L)** README says "there is no web flasher in this repository"
  (README:343-344) while `web/` and the Pages deploy exist. Reconcile.
- **B6 (L)** CI cleanup: the `sed -i ... main/main.cpp` for `APP_VERSION` is a
  no-op (no such macro, `build-release.yml:72-73`); `IDF_VERSION` env is unused by
  the container step; `idf.py fullclean` is fine in CI but no longer needed for
  target isolation now that the matrix is C6-only.
- **B7 (L)** `sdkconfig.defaults:23` sets `CONFIG_PARTITION_TABLE_FILENAME`
  redundantly (auto-derived from `..._CUSTOM_FILENAME`). The
  `CONFIG_BT_NIMBLE_TASK_STACK_SIZE` note (`:74-76`) claims the key is dropped when
  renamed, but the legacy alias is honored — generated `sdkconfig` has
  `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=6144`. Correct the comment so no one
  "fixes" a non-problem.

---

## Verified OK — do not churn

- Image fits: 1,969,376 B < 0x1F0000; partitions self-consistent for 4 MB.
- Echo-loop is real and the `PublishGuard` correctly suppresses device-originated
  reports from re-entering PRE_UPDATE (writable attrs route through the provider,
  `esp_matter_data_model.cpp:1036-1042`).
- `report()` is thread-safe (CHIP lock) and no-ops are `ESP_OK`, so there is no
  log spam for unchanged values.
- Temperature path uses integer math (`step * 50` centi-degrees) matching the
  BedJet's half-degree wire resolution (`bedjet_protocol.h:145-150`,
  `bedjet_matter.cpp:149-157`); the earlier 1 °F display offset is addressed.
- Status parser accepts exactly 20 bytes and `memcpy`s into a packed struct; no
  fragmentation (`bedjet_ble.cpp:641-662`).
- Fan step<->percent mapping is exact and clamped to 5–100%
  (`bedjet_protocol.h:154-169`).
- Thermostat `Heating|Cooling` + `ControlSequenceOfOperation 0x04` is internally
  consistent, and publishing only the active setpoint is the correct
  single-target model.
- Task stacks (NimBLE host 6144, BLE/bridge 6144, main 8192) are sane; queue-set
  capacity (9 ≥ 2 members) is safe.

---

## Execution order

1. **Phase 0 budget** — delete M1/R8/R9 dead code, apply M2, re-measure.
2. **R1** (handover idempotency) — highest reliability risk, touches boot.
3. **R2 + R3 + R4** (discovery mode, watchdog liveness, critical-section APIs) —
   the BLE layer with no abstraction to hide bugs; verify on a real BedJet.
4. **R5 + R11** (publish guard scope, CHIP lock for `get_val`).
5. **R6/R7/R10 and Phase 2** polish.
6. **Phase 4** packaging/docs, then a final size check.

## Acceptance criteria

- Clean `idf.py build`, no size warning; BLE handover succeeds on first or second
  retry with no extra `bedjet_ble` task and no repeated `NimBLEDevice::init`.
- Saved-MAC loss (flash a wrong MAC) recovers via an **active** scan.
- An idle (standby) BedJet keeps its link for well over 5 minutes with no
  "recycling link" log, while a real notify stall still recycles.
- No `taskENTER_CRITICAL_FROM_ISR`/`xTaskGetTickCountFromISR` in task paths; no
  FreeRTOS assertions during connect/notify.
- A controller write issued during a status publish is still sent to the BedJet.
- Release workflow produces exactly one C6 asset and the release body/web page
  reference only that.
