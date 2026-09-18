# ESP32 migration implementation status

This is the execution record for the phases in
[`ESP32_MIGRATION_SPEC.md`](ESP32_MIGRATION_SPEC.md). A phase is not marked
complete merely because its code exists: hardware and live-account gates stay
explicit.

| Phase | Implemented in this workspace | Verification / remaining gate |
|---|---|---|
| 0. Investigation/design | Spec and interface concepts retained as the source of truth | Complete before this implementation; no earlier test results were re-labelled |
| 1. Preservation/evidence | USB inventory, V1 pin/driver audit record, and an explicitly authorized read-only chip/flash/PSRAM/security/partition audit; no flash writes | Chip/flash/PSRAM/security/partition evidence is recorded; rear-label photo, TF-card/free-space, battery/RTC/PMU behavior, and full physical display/touch acceptance remain open |
| 2. V1 platform foundation | ESP-IDF project shell, partition policy, native V1 TCA9554 sequencing, SH8601 QSPI panel setup, FT3168 probe/read adapter, BOOT/touch GPIO setup, read-only PCF85063 calendar/validity decoder, LVGL 9.2.2 snapshot presenter with touch command queue, monotonic clock, guarded LittleFS initial-format/recovery policy, paired NVS generations, journal/recovery core, bounded logging contract | ESP-IDF 5.5.4 target build and native CMake/CTest pass; authorized serial bring-up reaches LittleFS mount, journal/cache initialization, SH8601 panel creation, LVGL readiness, BLE initialization, setup AP startup, and FT3168 frame input. Existing-volume mount failure preservation is source-guarded and host-policy tested; physical corruption/durability behavior, display colors/orientation, full-corner repeat, BOOT/PWR behavior, RTC backup validity (oscillator-stop flag observed), SD detection, and measured heap/power remain open gates |
| 3. Setup/cache | Bounded settings validation, transactional setup session, local setup HTML, ESP-guarded protected AP/HTTP handlers, embedded setup page, bounded Wi-Fi/SNTP/athlete verification before NVS commit, Intervals request builder/client, boot-time today/profile/ZWO sync, and strict ZWO cache normalization | User-reported prior board evidence: Wi-Fi, trusted SNTP, Intervals athlete HTTP 200 and settings surviving reboot. This laptop phase adds transactional on-device profile editing/retry and bounded AP shutdown code; physical settings/touch/expiry acceptance, secure-at-rest decision, and wider live Intervals calls remain gates |
| 4. BLE/safe ERG | FTMS/HRS parsers, byte-level commands, power capability negotiation, command generations and failure classification, plus an ESP-IDF NimBLE central with bounded address/type-qualified scan candidates, explicit selection and persisted address/type identities, two-link reconnect generations, FTMS/HRS GATT discovery, feature/range reads, required CCCD subscriptions, bounded notification delivery, and indication-only control acknowledgement | Exact Hub/strap behavior, safety-release measurement, command latency, and 20 reconnect cycles require hardware; the app intentionally does not auto-select or auto-resume |
| 5. Workout/simulation/recording | Three clocks, step/ramp engine, deterministic terrain, actual-power physics, 1 Hz sample wire format, CRC journal and interrupted-prefix recovery, coordinator safety rules, and an ESP runtime task that filters BLE generations, restores cached Preview, guards Start/Resume, and preserves an interrupted current journal | 60-minute Hub+Garmin ride, power-cut target, resource/endurance measurements require hardware |
| 6. FIT/history/upload | FIT activity structure with CRC/timer/lap/session/activity, persisted journal UTC anchors, post-ride and recovered-session FIT publication, finalized-session publication reconciliation, local summaries, SHA-256 artifact verification, FIT-pending reconciliation, durable retry queue with reboot-safe relative retry waits, bounded remote activity IDs, an outside-the-ride upload worker, a bounded offline recent-history screen, and local history detail presentation | Official Garmin Python FIT SDK 21.214.0 independently validates complete, partial, paused and journal-recovered fixtures. Host tests cover persisted Retry-After rebasing and finalized-session enumeration; physical history browsing after reboot, export, and live upload/dedup/lost-response tests remain integration gates |
| 7. Release hardening | Release checklist and explicit unresolved-gate record; native and ESP-IDF build/size evidence captured | 90/120-minute/4-hour soaks, storage/security/time/touch tests, final production partition sizing, and secure-boot/encryption procedure remain open |
| 8. P1 | Not started by design | Rich controls/library/export/CPS/ramp-test are separate scope; GPX/free-ride/coaching stay deferred |

The host test suite is evidence for portable contracts only. It is not evidence
that the ESP32 radio, display, power circuitry, FIT decoder ecosystem, trainer
fail-safe, or Intervals service interoperates correctly.

## Laptop-only continuation — 2026-09-17

The parent Git repository reports this entire project as untracked. Existing
source/configuration was preserved before edits in
`/tmp/trainer-loop-esp32-before-20260917.tgz`; no reset, checkout, flashing,
serial connection, provisioning repetition, erase or eFuse operation occurred.
The user reports V1 panel/touch initialization, successful provisioning and
persistent settings, and runtime startup with the 6144 stack setting. These
are prior board observations, not new hardware results from this phase.
`SyncState::kEmpty` (3) means no supported workout today, not sync failure.

- Expanded deterministic parsing/boundary, clock, safety, reboot, upload and
  storage contracts; reusable `ITrainerController` replay drives production
  FTMS/HRS parsers and command gates. All 8,192 supported FTMS flag combinations
  and their truncated prefixes are covered.
- Added FTP/rider mass/bike mass/Crr/CdA draft editing, Save/Cancel, persistence
  rollback, local profile overrides, retry, explicit empty-cache messages,
  and a separate End & Save control while paused. Active/paused/finishing
  sessions reject settings/network operations.
- Regression fixes: non-rewinding clock observations; advancing UTC between
  syncs with fixed ride anchors; pause on subscription loss; missing FIT
  summaries remain invalid; disconnects generate FIT stop events; recovered
  durations derive from durable records; event logs publish atomically;
  ambiguous 422 text cannot become duplicate success; in-flight upload
  deadlines are reset for the new boot's monotonic epoch; persisted Retry-After
  and transient waits are rebased from durable relative delays while FIT hash
  and journal-captured destination athlete remain unchanged; interrupted
  finishing can retry
  locally without repeating trainer stop; finalized sessions are reconciled
  after an interrupted artifact publication; returning Home clears the old
  workout and a new ride resets its artifact-publication state; remote profile
  updates are staged until a complete today/ZWO sync succeeds.
- LittleFS formatting is now permitted only when the labeled partition is
  proven entirely erased. A mount failure on an existing partition returns a
  recovery-preserving error and never formats unsynced rides.
- Host-only short-write/ENOSPC injection covers journal, cache, FIT and queue
  writes. Test controls do not compile into ESP firmware. No Wi-Fi is needed
  for local cache/history/recording; upload eligibility explicitly gates Wi-Fi.
- Reviewed setup bounds, secrets and encryption policy. Added AP expiry
  shutdown scheduling, independent RNG bytes for setup credentials/token,
  and visible plaintext-development-NVS labeling. Physical unlock/re-entry,
  actual AP shutdown, platform log/crash review, and production encryption
  decisions remain release gates.

`make test`, explicit Debug CMake build and CTest pass (2/2, including the
external SDK validator). The latest `idf.py build` also passes: app
`0x2207e0`, free `0x1df820` (47%) in the current app partition; bootloader
`0x5160` with 36% free. No physical acceptance gate is closed by these results.
Full evidence, limitations, changed-file groups and the exact home sequence:
[Laptop validation](validation/LAPTOP_VALIDATION.md).

The user additionally reports that the Garmin HR monitor appears during scan
and can be connected, but no HR values are visible on the device screen; the
trainer does not appear during scan. This is an open hardware/UI integration
finding, not laptop evidence and not a release-gate pass.

## Development replacement follow-up — 2026-09-18

The authorized development replacement completed after a fresh 16 MiB
flash/security check and partition-sector read. Existing NVS, rides, cache and
storage were intentionally discarded without a credential-bearing backup.
Full erase and manifest write completed, and post-write verification matched
the bootloader, application and partition table. The bootloader updated the
initially erased OTA metadata on first boot, so the raw initializer hash is not
expected to remain unchanged. Boot evidence confirms clean LittleFS mount,
SH8601/LVGL readiness and protected web setup at `192.168.4.1`; no panic or
watchdog appeared in the bounded monitor window. See
[maintenance evidence](hardware/MAINTENANCE-2026-09-17.md).

BLE was not initialized on the unprovisioned path. Private web setup must be
completed before scanning the trainer or HR monitor; no resistance workout is
authorized until FTMS/HR telemetry, acknowledgements and pause/stop behavior
are separately validated.

## UI and live-service follow-up — 2026-09-18

The first supervised ride confirmed trainer ERG control, pause/resume, Garmin
heart-rate telemetry, local history creation, and an uploaded Intervals.icu
activity. The observed UI issues were stale post-save timing, ambiguous End &
Save feedback, duplicate Devices actions, small pairing/history targets,
non-actionable history rows, and a pairing scan error while automatic GATT
setup was still in progress.

The implementation now freezes the completed-session clock, presents a static
RIDE SAVED summary, exposes clickable history rows and a detail view, keeps one
Devices entry, uses larger pairing/history buttons, and reports the BLE setup
conflict as a retryable state instead of leaving the screen apparently locked.
The completion and history detail views also distinguish uploaded, pending,
retrying, blocked, and failed upload states using user-facing labels.

The active ride snapshot now exposes interval position, interval time remaining,
and whole-workout percentage. The active screen uses those values alongside
active elapsed time instead of showing raw plan seconds.

The cache now keeps every supported workout returned for today. When more than
one is available, Home/Preview shows an explicit `1/N` choice, offers a large
NEXT button, and cycles the selection without changing the single-workout flow.

The History screen's retry action now has its own command. `RETRY UPLOAD`
re-queues only durable authentication/transient upload failures and schedules
the upload worker immediately; Home `RETRY` remains the bounded Wi-Fi/time/
workout-sync action. This prevents a history retry from incorrectly producing
an Intervals workout-sync error.

Checkpoint `79f587b` was built and flashed to the application slot only at
`0x20000` (SHA-256
`144ed3dcd6e9e3b9cc50d4ff841d4a7cd3fe41ae2753401e4070bf56a9a151e1`). A
bounded monitor window confirmed app version `79f587b`, the project partition
table, LittleFS mount, LVGL readiness, Wi-Fi recovery, three Intervals HTTP 200
responses, and no panic/watchdog. The user has since confirmed device
connection, History navigation, and a static completed-ride elapsed timer.
Remaining work is physical acceptance of the interval-progress and
multiple-workout views, exact trainer/HR pairing behavior under reconnect,
safe ERG acknowledgements, and the release-gate endurance and reconnect tests
in the checklist.

Checkpoint `0ce346c` was then rebuilt and flashed only to application offset
`0x20000` (SHA-256
`9e3ac95d1b352287a82045bdcff0eaa626845b2ca243de7698dddf991373f1de`). The
image is `0x220b70` bytes with `0x1df490` free (47%). A bounded monitor window
confirmed the new app version, existing partition table, LittleFS mount,
display readiness, Wi-Fi recovery, three Intervals HTTP 200 responses, BLE/GATT
startup, and no panic/watchdog output. No bootloader, partition table, NVS, or
storage write was performed.

Checkpoint `11c1ddc` adds the Home date/cache status line and a two-line
workout context (`Today`/`Preview`, duration and interval count), plus the
test-first date-label contract. It also keeps the Home synchronization retry
separate from the History upload retry. The clean ESP build is `0x220f70`
bytes with `0x1df090` free (47%); SHA-256 is
`e6bc78e18a9009cd9bb7bf180d4e3d147553957deca04ae0b022e596baac7005`.
Only application offset `0x20000` was written. The bounded monitor confirmed
app version `11c1ddc`, the project partition table, LittleFS mount,
SH8601/LVGL readiness, automatic Wi-Fi recovery, three Intervals HTTP 200
responses, BLE/GATT startup, and no panic/watchdog output. A live camera
spot-check showed the inset status line, selected-workout context and larger
action targets on the physical display; formal touch/readability acceptance
remains open.

Checkpoint `796c401` adds a compact eight-bar interval profile to Home/Preview,
derived from the cached workout's relative segment targets. The profile is
render-only and does not change workout control or selection. Test-first host
coverage verifies normalized two-segment profiles. The clean ESP image is
`0x221380` bytes with `0x1dec80` free (47%); SHA-256 is
`123bfb3fca44d4f826f3c21b5cda66aaa5bda8b35e6e47f43cdfa8ce5a1542df`.
Only application offset `0x20000` was written. Boot evidence confirmed app
version `796c401`, LittleFS/LVGL readiness, Wi-Fi recovery, BLE/GATT discovery,
three Intervals HTTP 200 responses, and no panic/watchdog output. The live
camera spot-check showed the bars fitting above the shifted action tiles;
formal touch/readability acceptance remains open.

Checkpoint `fef531e` adds long-plan coverage for the same profile mapping. The
host smoke test, Debug CMake/CTest suite, strict Clang `-Werror` build, and
ASan/UBSan run all pass. The sanitizer rerun omits macOS's unsupported leak
detector and reports no memory or undefined-behavior findings. This is a
test-only checkpoint; at that point the device continued to run the flashed
application image `796c401`. Physical touch/readability, reconnect, control-acknowledgement,
and endurance gates remain open.

Checkpoint `6a6bfea` makes each successful daily sync replace the cached
workout set for that local date. Stale same-day event records are pruned only
after all downloaded records have been written; a successful empty response
clears that date, while network/download/write failures do not prune the prior
set. Host tests cover replacement, empty responses, and a failed write that
must not prune existing records. The clean ESP build is `0x221af0` bytes
with `0x1de510` free (47%); SHA-256 is
`f59073d360236474e7bad3e88fab20a3d2753d426579e4818aafc4d1cfa30e6a`.
At the time of this laptop-only checkpoint, the device remained on `796c401`.

The clean image was subsequently flashed only to application offset `0x20000`
after the final Git checkpoint. Boot evidence identifies app version `713e2c0`,
confirms the existing project partition table, LittleFS mount, SH8601/LVGL
readiness, Wi-Fi recovery, three Intervals HTTP 200 responses and BLE/GATT
discovery/subscription activity, with no panic or watchdog output. The known
PCF85063 oscillator-stop warning remains a hardware time-validity gate.

Test checkpoint `45c9258` extends the cache contract with invalid-date and
empty-directory replacement boundaries; it does not change firmware behavior.

Checkpoint `23ac626` serializes persisted trainer/HR reconnect attempts so the
NimBLE central starts at most one `ble_gap_connect` operation per poll. This
removes the avoidable concurrent-connect `EBUSY` path that could surface as a
BLE scan/setup conflict while the two saved links were being restored. The
clean ESP-IDF image is `0x221b10` bytes with `0x1de4f0` free (47%); SHA-256 is
`c5aa36238975c471e56a9f715d9ab1814358663fcfb2b52423ac22930aa92159`.
Only application offset `0x20000` was written. The bounded monitor identified
app version `23ac626`, the project partition table, LittleFS/LVGL readiness,
automatic Wi-Fi recovery, BLE/GATT startup and no panic/watchdog output; no
concurrent-connect deferral appeared during the observed startup window. The
known PCF85063 oscillator-stop warning and all supervised physical pairing,
acknowledgement, safety, and endurance gates remain open.

A live camera spot-check of the flashed image shows the compact connection line
and four Home action tiles inside the display bezel with no obvious clipping.
This is visual evidence only; direct touch targets, readability under intended
lighting, and the pairing/reconnect workflow still require hands-on acceptance.

Checkpoint `2fba6d3` makes `list_for_date` sort cached workouts by durable event
ID, so the Home/Preview **NEXT** order is stable across filesystem enumeration
and reboot. Test-first coverage saves two same-day records in reverse creation
order and verifies deterministic ordering. The clean ESP-IDF image is
`0x2220a0` bytes with `0x1ddf60` free (47%); SHA-256 is
`b323236c9e996f668a9922e8e9df66a142f1cf1020e79499a9433c598e0bb646`.
Only application offset `0x20000` was written. The bounded monitor identified
app version `2fba6d3`, the expected project partition table, LittleFS/LVGL
readiness, automatic Wi-Fi recovery, three Intervals HTTP 200 responses and
BLE/GATT setup with no panic/watchdog output or concurrent-connect deferral in
the observed window. Physical selection, touch/readability, control-safety,
and endurance gates remain open.

Checkpoint `8ac2336` makes daily cache replacement roll back the prior valid
same-date set if any new record write or pruning step fails. It snapshots files
that would be overwritten, restores them after a failed replacement, and
removes partial new records; host coverage now exercises a failure after the
first of two new records. The clean ESP-IDF image is `0x222cd0` bytes with
`0x1dd330` free (47%); SHA-256 is
`0475570e50382541d0be13a04a1dad735ea0098e155e842744489b11bdcb2f36`.
Only application offset `0x20000` was written. The bounded monitor identified
app version `8ac2336`, the project partition table, LittleFS/LVGL readiness,
automatic Wi-Fi recovery, three Intervals HTTP 200 responses and BLE/GATT
setup with no panic/watchdog output. Physical cache selection, touch/readability,
control-safety, and endurance gates remain open.

Checkpoint `c4700b2` makes a foreground BLE scan conflict recoverable: when the
user opens Devices while a saved-link GATT setup is still settling, the pairing
screen shows `PAIRING | WAITING`, explains that scanning will start automatically,
and retries every 250 ms. Back cancels the wait; a non-conflict transport or
memory error remains visible for manual diagnosis. Host tests, strict Clang,
ASan/UBSan, and a clean ESP-IDF build pass. The `0x222f80`-byte image has
`0x1dd080` bytes free (47%) and SHA-256
`391338c1ade1328f52e1d53390ad05cd046768ff4f077cef02850e30aa4695b6`.
It was flashed only to application offset `0x20000`; no bootloader, partition
table, NVS, LittleFS, or credential-bearing backup was touched. The bounded
boot monitor identified app version `c4700b2`, the project partition table,
LittleFS/LVGL readiness, Wi-Fi lease acquisition, three Intervals HTTP 200
responses, BLE/GATT setup, and no panic/watchdog output. Direct Devices-scan,
touch/readability, control-safety, and endurance acceptance remain physical
gates.
