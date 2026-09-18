# Laptop validation — 2026-09-17

No board access, flashing, serial resets, provisioning repetition, trainer/HR
radio interaction or eFuse/security changes were performed. Existing files
were preserved in `/tmp/trainer-loop-esp32-before-20260917.tgz` before editing.
The project was entirely untracked in its parent repository, so this archive,
not an empty Git diff, is the source baseline.

## Reproduce

From `/Users/david.ramirez/Projects/Personal/trainer-loop-esp32`, in a fresh
shell without `IDF_PATH` set:

```sh
python3 -m venv .venv-fit
.venv-fit/bin/pip install -r host/tests/fit-requirements.txt
make test
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
source /Users/david.ramirez/esp/esp-idf/export.sh
idf.py build
```

`make test` includes the native contracts and SDK validation. CTest has two
entries; the first contains the deterministic C++ cases, the second generates
and decodes four FIT fixtures. The external decoder is pinned to
`garmin-fit-sdk==21.214.0`; use `-DFIT_PYTHON=/path/to/python` for another
installation. Test executables use assertions: run the prescribed Debug build.
All commands above passed. Firmware app size: `0x21c5d0`; free app partition:
`0x1e3a30` (47%). Bootloader: `0x5160` (36% free). Firmware was only built.

Firmware replacement is currently blocked on partition compatibility. The
earlier live-table evidence records `factory`/`ota_0` application offsets and
SPIFFS storage that differ from the current `partitions.csv` `app0`/`app1`
offsets and LittleFS storage. That evidence predates later bring-up and is
not a fresh read. Do not run `idf.py flash` or `idf.py app-flash` until a new
read-only partition-sector check, an old-image backup plan, and an explicit
NVS/storage retention decision are complete. A table/app write without a
storage initialization decision may leave old bytes in the new LittleFS
region, which the non-destructive mount guard correctly refuses to format.

## Coverage and boundaries

| Area | Laptop evidence | Still unverified |
|---|---|---|
| Workout | Invalid/oversized/non-finite duration/power, source/repeat limits; ramp, step and IntervalsT boundaries | Real account dialects and all workout variants |
| Clocks/safety | Pause freezes active/plan; wall continues; monotonic regression; UTC advances; ride UTC anchor survives correction; start guards; failed resume; stale/subscription/disconnect pause; no reconnect/reboot auto-resume | Trainer's physical resistance release/hold and latency |
| Replay | Reusable fake controller drives production connection generations, parsers and command gate; ACK/NACK/late/timeout; reconnect/stale notifications; coordinator pause/stop traces; 8,192 FTMS flag combinations and truncated prefixes; HRS cases | NimBLE radio/CCCD interoperability, exact Hub/strap captures, measured ERG |
| Upload | 401/403 blocked; 408/429/5xx retry; timeout/DNS/TLS represented by the runtime's status-zero transport outcome; Retry-After preserved; FIT identity and journal-captured destination athlete preserved across reboot; ordinary/ambiguous 422 rejected; exact duplicate fixture; lost response/reboot retains identity; no sending while offline/riding | Actual DNS/TLS stacks, authorized service upload, real duplicate schema and exactly-once remote behavior |
| Storage | ENOSPC/short writes at chunk/manifest/FIT/cache/queue boundaries; every cut position in second chunk retains first; corrupt newest manifest falls back; both corrupt preserve directory and fail; finalized-session enumeration; guarded initial-format policy; orphan FIT temporary file; failed upload result survives boot; idempotent FIT/queue retries; failed finishing retries without another trainer stop | LittleFS power-cut durability, directory rename durability, fragmented/full volumes, TF removal, reserve/endurance measurements |
| UI/profile | Portable validation, active-session lock, persistence failure leaves memory unchanged; restored five-value profile and local overrides; ESP LVGL/runtime compilation | Screen layout, touch accuracy, queue behavior and timing on the physical display |
| Security | Host expiry/close checks; Host/Origin/CSRF/rate-limit existing contracts; bounded numeric/string inputs; source-level secret flow and TLS checks; expiry timer compiles | Physical unlock/re-entry, actual AP expiry, platform logs/crashes, encrypted-at-rest release procedure |

The simulator cannot prove physical fail-safe behavior. Storage injection is
host-only and operates on real temporary files; it is not a LittleFS emulator.
An offline cached ride still requires trusted time: an invalid RTC must block
Start until time is established. Cached files remain preserved during that block.

## Authorized development replacement — 2026-09-18

After this laptop-only record, the user authorized discarding all development
device data and declined a credential-bearing backup. A fresh no-stub inspection
confirmed ESP32-S3 v0.2, 16 MiB flash, secure boot/encryption disabled, and the
current `partitions.csv` layout. Full-chip erase and the generated manifest
write completed. Bootloader, application and partition table verification
passed; the bootloader populated OTA metadata on first boot, so the erased OTA
initializer is expected to change.

Bounded serial evidence confirmed the new bootloader/application, clean LittleFS
initialization, SH8601/LVGL readiness and protected web setup at
`192.168.4.1`, with no panic or watchdog during the observation window. The
unprovisioned path returns before BLE initialization; private web setup is
required before trainer/HR scanning. No resistance workout was attempted.

## Independent FIT results and identity

`host/tests/fit_fixtures.cpp` generates complete, partial, paused and recovered
files; recovery uses the journal's durable prefix without a final manifest
commit. `host/tests/validate_fit.py` uses the unmodified
[official Garmin Python FIT SDK](https://github.com/garmin/fit-python-sdk).
It independently checks CRC (including deliberate corruption rejection), UTC,
start/pause/resume/stop events, lap/session/activity elapsed/active totals,
missing HR versus valid zero power/speed, descent, distance, and repeated hashes.
Generated artifacts are in `build-host/fit-validation/`.

This caught and fixed missing metrics encoded as zero and omitted disconnect
pause events. Recovery tests also caught zero durations in unfinished manifests.
The FIT identity remains manufacturer **255 (`development`)**, product **0**,
and unspecified serial (0). This matches the SDK's development identity;
it does not impersonate Garmin. Product registration/identity assignment remains
a release decision; see Garmin's
[manufacturer ID guidance](https://thisisant.developer.garmin.com/pages/developer/resources/tech-bulletin/manufacturer-id/index.html).
No production identity has been assigned or claimed.

## Security and release constraints

- API keys/home Wi-Fi credentials are absent from UI snapshots and new messages.
  Profile edits never include secret fields. Setup's temporary join password is
  displayed only for the existing protected-AP provisioning flow; stored API and
  home Wi-Fi secrets are not displayed. Error output does not contain raw bodies.
- Requests are bounded (4 KiB setup body, 256 KiB cloud response), credentials
  remain write-only, certificate-bundle TLS verification is enabled, and HTTP
  redirects are disabled. These are source observations, not penetration tests.
- Added AP expiry shutdown scheduling; platform RNG now supplies independent
  bytes for the setup password and 128-bit CSRF token. Test physical shutdown and
  concurrent validation near expiry later. Physical unlock/re-entry remains open.
- Current NVS, flash encryption and secure boot are **disabled** in development;
  crash dumps to UART/flash are disabled. Setup page/settings identify plaintext
  development storage. Platform Wi-Fi/TLS/NimBLE logging and crash paths still
  require a physical secret-leak review; application log review alone is not proof.
- LittleFS formatting is now guarded by a full erased-partition check. The
  runtime passes `format_if_mount_failed` only for a demonstrably empty labeled
  partition; any existing bytes make mount failure non-destructive and return a
  recovery-preserving error. Host policy tests cover the decision, but do not
  prove LittleFS power-cut/directory durability. Do not format a device
  containing unsynced rides.
- The exact plain-text duplicate fixture is a conservative development contract,
  not a verified Intervals response schema. Unrecognized 422 responses remain
  errors. Rebooted retry-wait entries now rebase the persisted relative wait,
  including Retry-After, while preserving FIT identity and destination athlete;
  live service/backoff policy validation remains open.
- No eFuses were burned and no irreversible secure-boot, encryption or debug
  lockdown configuration was enabled.

## Application-only follow-up — 2026-09-18

After the supervised ride feedback, the portable selector and interval-progress
contracts were extended test-first and the ESP image was rebuilt from
checkpoint `79f587b`. `make test` and Debug CTest remain 2/2, including
independent FIT decoding. The image is `0x220a50` bytes with `0x1df5b0` free
(47%); SHA-256 is
`144ed3dcd6e9e3b9cc50d4ff841d4a7cd3fe41ae2753401e4070bf56a9a151e1`.

Only the application slot at `0x20000` was written. The monitor confirmed the
new app version, the existing project partition table, LittleFS mount, LVGL
startup, automatic Wi-Fi recovery, three Intervals HTTP 200 responses, and no
panic/watchdog. No credential-bearing backup was made. The user has confirmed
device connection, History navigation, and a static completed-ride timer.
Physical acceptance is still required for NEXT selection, interval-progress
readability, and the scan-conflict retry path.

The follow-up checkpoint `0ce346c` adds the explicit History `RETRY UPLOAD`
command so a saved-ride retry cannot invoke workout synchronization. Host
tests and the Debug CTest suite remain 2/2. The rebuilt application is
`0x220b70` bytes with `0x1df490` free (47%); SHA-256 is
`9e3ac95d1b352287a82045bdcff0eaa626845b2ca243de7698dddf991373f1de`.

Checkpoint `11c1ddc` adds the Home date/cache status line and a two-line
workout context with duration and interval count. The date-label helper was
added test-first; `env -u IDF_PATH make test`, the Debug CMake build and CTest
2/2 (including the independent FIT validator) pass. The clean ESP image is
`0x220f70` bytes with `0x1df090` free (47%); SHA-256 is
`e6bc78e18a9009cd9bb7bf180d4e3d147553957deca04ae0b022e596baac7005`.
Only application offset `0x20000` was written. Monitor evidence confirms
app version `11c1ddc`, the project partition table, LittleFS/LVGL startup,
Wi-Fi recovery, three Intervals HTTP 200 responses, BLE/GATT startup and no
panic/watchdog output. A live camera preview showed the inset status line and
the selected-workout context on the real display; direct physical readability
and touch acceptance remain open.

Checkpoint `796c401` adds a compact eight-bar interval profile to Home/Preview,
derived from relative cached segment targets. The clean image is `0x221380`
bytes with `0x1dec80` free (47%); SHA-256 is
`123bfb3fca44d4f826f3c21b5cda66aaa5bda8b35e6e47f43cdfa8ce5a1542df`.
`env -u IDF_PATH make test`, the Debug CMake build and CTest 2/2 pass with
test-first profile assertions. Only application offset `0x20000` was written.
The monitor confirmed app version `796c401`, LittleFS/LVGL startup, Wi-Fi
recovery, BLE/GATT discovery, three Intervals HTTP 200 responses and no
panic/watchdog output. A live camera preview showed the bars above the shifted
Home/Preview action tiles; direct physical touch/readability acceptance remains
open.

Checkpoint `fef531e` adds a long-plan profile assertion so ten source segments
still map to all eight display bars with the final bar at the maximum level.
The host smoke test, Debug CMake/CTest suite, strict Clang `-Werror` build,
and ASan/UBSan run pass. The sanitizer run uses the macOS-compatible
`halt_on_error` configuration; LeakSanitizer is unavailable in this runtime.
This checkpoint changes tests only. At that point the flashed device image
remained `796c401`.

Checkpoint `6a6bfea` fixes daily cache replacement. A successful Intervals
sync now removes stale same-day event records only after the new set is
written; a successful empty response clears that date, and a failed write does
not prune existing records. Host tests cover both paths. The clean
ESP-IDF image is `0x221af0` bytes with `0x1de510` free (47%); SHA-256 is
`f59073d360236474e7bad3e88fab20a3d2753d426579e4818aafc4d1cfa30e6a`.
At the time of this laptop-only checkpoint, the device still ran `796c401`.

The image was then flashed only to application offset `0x20000` from the clean
final checkpoint. Esptool wrote `0x221af0` bytes and verified SHA-256
`b9d9cfadf1965125b6167da67af0cd36ca7e740a1c1cf661e6ad0f8a16af8a0b`.
The bounded monitor identified app version `713e2c0`, the existing project
partition table, LittleFS mount, SH8601/LVGL readiness, Wi-Fi recovery, three
Intervals HTTP 200 responses and BLE/GATT discovery/subscription activity,
with no panic/watchdog output. No bootloader, partition table, NVS or storage
write was performed. The PCF85063 oscillator-stop warning remains open.

Test checkpoint `45c9258` adds invalid-date and empty-directory replacement
boundaries; it does not change the firmware image.

## Exact home testing sequence (deferred)

1. **Do not re-provision.** Bring the existing board, exact Zwift Hub One and
   Garmin strap home. Record their firmware/model identities. Keep a rider able
   to stop pedaling immediately and use low workloads for all initial tests.
2. The board now runs checkpoint `713e2c0` from an application-only write at
   `0x20000`; existing NVS and storage were left untouched, and no
   credential-bearing backup was made. Never use `erase-flash`, change security
   settings, or re-provision as part of this validation record.
3. Boot normally. Confirm settings persist, V1 panel orientation/colors and four
   touch corners, no panic/watchdog, and adequate heap. Do not enter setup merely
   to repeat the previously successful Wi-Fi/SNTP/athlete HTTP-200 validation.
4. Open **Settings** from Home/Preview. For each of FTP, rider kg, bike kg, Crr and
   CdA, change one increment using `+`/`-`, use **NEXT FIELD**, then **SAVE**.
   Reboot and verify all five values. Restore desired values. Change a draft and
   use **BACK / CANCEL**; reopening must show saved values. Check bounds. Never
   photograph or paste a screen containing setup credentials.
5. Tap **RETRY** with Wi-Fi available. With no supported workout today, expect an
   empty-workout message, not an authentication error. Later, with a supported
   cycling workout scheduled by the user, retry and verify Preview duration and
   FTP-derived targets. With Wi-Fi unavailable, retry must show a connection
   error and retain the cache. Establish valid RTC time before attempting an
   offline cold boot; if the RTC is invalid, Start must stay blocked.
6. Open **DEVICES** and explicitly choose the exact trainer/strap address/type.
   Confirm power-range/features and telemetry/control subscriptions; do not pair
   by name/RSSI alone. Wake the HR strap and check contact, disconnect and stale
   display behavior without interrupting trainer recording.
7. Start a conservative short workout only with fresh power and trusted time.
   Verify target versus actual, cadence/HR and virtual metrics. Pause: active/plan
   clocks freeze, elapsed continues. Settings/retry must be unavailable. Verify
   physical resistance release; Resume requires explicit control reacquisition
   and conservative ramp-in. Pause again, then **END & SAVE**. A failed local finish offers **RETRY SAVE**;
   it must preserve the same session and never restart the trainer. Return Home
   only after local artifacts are saved, then verify a second ride can start
   and produces its own FIT/history entry.
8. Test stale power, missing notifications, NACK/timeout and supervised disconnect
   separately. **Stop pedaling when resistance is unknown.** Reconnect must remain
   paused; require fresh power and explicit Resume. Measure last-target hold and
   pause/stop behavior before increasing workload. Repeat at least 20 reconnects.
9. Complete an offline ride, browse history after reboot, restore Wi-Fi and inspect
   upload status/remote activity ID. Independently decode the real FIT; compare
   UTC, pauses, totals and missing metrics with the ride record. Run authorized
   lost-response/duplicate and HTTP-failure tests against the actual service;
   do not change file timestamps/hash to force upload acceptance.
10. Only after backup and explicit power-cut-test authorization, test cuts during
    sample/manifest/FIT/upload publication. Reboot must save a partial ride and
    never restart resistance. Check sample loss against the five-second target
    and confirm no lost/duplicated local artifacts or uploads. Do not run damaged
    mount tests until the format-on-failure release blocker above is resolved.
11. Finish the open checklist: ≥60-minute trainer/HR ride; 90/120-minute and 4-hour
    soaks; five retained 120-minute sessions; full/fragmented storage; RTC/battery/
    TF behavior; touch with intended lighting/gloves/sweat. Setup expiry/unlock
    acceptance can use a dedicated maintenance session later, without resetting
    or replacing the already working credentials. Record evidence per gate.

## Changed files

- Tests/build: `host/tests/test_core.cpp`, new `migration_contracts.h`,
  `ui_settings_contracts.h`, `replay_contracts.h`, `trainer_replay.h`,
  `storage_contracts.h`, `fit_fixtures.cpp`, `validate_fit.py`,
  `fit-requirements.txt`; `host/CMakeLists.txt`, `Makefile`, `.gitignore`.
- Portable core headers/sources: `clock`, `time_service`, `ui`, `upload_queue`;
  `ble_manager.h`, new `storage_io.h`; `state_machine.cpp`, `intervals.cpp`,
  `fit_encoder.cpp`, `journal.cpp`, `fit_store.cpp`, `workout_cache.cpp`,
  `settings.cpp`, finalized-session enumeration/reconciliation.
- ESP runtime: `main/firmware_app.h/.cpp`, `storage_mount.cpp/.h`,
  `intervals_sync.cpp`, `lvgl_presenter.cpp`,
  `provisioning.h/.cpp`, `web/setup.html`.
- Documentation: `README.md`, `docs/IMPLEMENTATION_STATUS.md`,
  `docs/validation/RELEASE_CHECKLIST.md`, this evidence/home-test record.
