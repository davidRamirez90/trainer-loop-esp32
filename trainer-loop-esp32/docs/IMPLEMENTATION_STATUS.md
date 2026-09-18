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

A static audit of the current `main/` and portable-core logging paths found no
`ESP_LOG*`, `printf`, or equivalent output that includes Wi-Fi credentials, API
keys, Authorization headers, raw HTTP bodies, or setup tokens. Physical
crash-output review remains open because it requires exercising the mounted
device and its reset/recovery paths.

The pairing rows now reserve two lines: the role and device name are shown
first, followed by the complete address and RSSI. This keeps the selectable
identity visible when several devices advertise similar names and avoids the
previous last-two-byte, clipped single-line presentation. The change is
render-only and does not alter address/type selection or connection policy.

Checkpoint `6244923` was rebuilt as a clean `0x222fb0`-byte image with
`0x1dd050` bytes free (47%) and SHA-256
`57ab51736f74a41f2f642f8fa25ce7266361e61857b89c51f4d1c2811af183a5`.
It was flashed only to application offset `0x20000`. The bounded monitor
identified app version `6244923`, the project partition table, LittleFS/LVGL
readiness, automatic Wi-Fi recovery, three Intervals HTTP 200 responses,
BLE/GATT setup, and no panic/watchdog output. The two-line pairing layout still
needs direct physical readability and touch acceptance.

The host-safe fallback build now also compiles every `main/*.cpp` translation
unit with Clang `-Werror`; ESP-only retry state and startup-sync bookkeeping are
guarded from non-ESP unused-variable diagnostics. This is a build-hygiene fix
only and does not change the ESP runtime behavior.

Checkpoint `6aba925` was rebuilt after the fallback warning fix as a clean
`0x222fb0`-byte image with `0x1dd050` bytes free (47%) and SHA-256
`7b6797986fc608bf72f8020223c8cc89969458192d652fc9736c69e372703e03`.
It was written only to application offset `0x20000`; esptool reported a
verified hash and performed the normal post-write reset. The bounded monitor
then captured LittleFS/LVGL readiness, three Intervals HTTP 200 responses,
BLE/GATT setup, and no panic/watchdog output. No bootloader, partition table,
NVS, LittleFS, erase, or credential-bearing backup action was performed.
The physical pairing-row readability/touch check, explicit trainer/HR
selection, control acknowledgement, pause/stop safety, and endurance gates
remain open.

Checkpoint `d1e1e64` fixes compact link-state classification so `disconnected`
cannot match the `connected` substring and render a lost link as `OK`. A
test-first portable contract covers disconnected, connected, and reconnecting
labels. The clean ESP-IDF image is `0x222fa0` bytes with `0x1dd060` bytes free
(47%) and SHA-256
`7427bf1c90a0fe4ba1467fc49fcc8a7fab16344a06ae72fa095a7c7513231688`.
Only application offset `0x20000` was written. The bounded monitor identified
app `d1e1e64`, the expected project partition table, LittleFS/LVGL readiness,
automatic Wi-Fi recovery, three Intervals HTTP 200 responses, BLE/GATT setup,
and no panic/watchdog output. No bootloader, partition table, NVS, LittleFS,
erase, or credential-bearing backup action was performed. Physical display
readability/touch and trainer/HR safety gates remain open.

A live camera spot-check after this write showed the active ride layout and
large action controls inside the display bezel. The feed was too soft to prove
small-text readability or touch accuracy, so those remain hands-on gates.

Checkpoint `af53660` makes the Home cache line report the actual Intervals sync
state instead of treating Wi-Fi association as a successful cloud sync. It
distinguishes `Synced`, `No workout today`, `Sync failed`, `Auth blocked`, and
`Wi-Fi online`, while preserving the offline cached state. The test-first
portable contract covers every sync-state label. The clean ESP-IDF image is
`0x2230b0` bytes with `0x1dcf50` bytes free (47%) and SHA-256
`8e582c9b7634fd973e3e23c181d4b65dfa19b2ef714adad7da015a1dc5da1ab2`.
Only application offset `0x20000` was written. The bounded monitor identified
app `af53660`, the expected project partition table, LittleFS/LVGL readiness,
automatic Wi-Fi recovery, three Intervals HTTP 200 responses, BLE/GATT setup,
and no panic/watchdog output. Physical display readability/touch and trainer/HR
safety gates remain open.

The follow-up camera spot-check showed the compact connection line, the
workout context, four distinct Home tiles, and one `START` action inside the
bezel without obvious clipping. Text was readable enough for layout review;
touch accuracy and disconnect-state transitions still require hands-on use.

Checkpoint `718d19e` routes the actual Intervals sync result through the
visible Home message line, so a successful fetch now reads `Synced` and an
empty supported day reads `No workout today`; detailed failure text remains
available for diagnosis. Host tests, CTest 2/2, and strict Clang `-Werror`
fallback syntax validation pass. The clean ESP-IDF image is `0x223090` bytes
with `0x1dcf70` free (47%); SHA-256 is
`fc15d9995ed297164b14123609fda55b7d831e74304ca5540b40b0e609960798`.
It was written only to application offset `0x20000`; no bootloader, partition
table, NVS, LittleFS, erase, or credential-bearing backup action was performed.
The bounded monitor identified app `718d19e`, the project partition table,
LittleFS/LVGL readiness, automatic Wi-Fi recovery, three Intervals HTTP 200
responses, BLE/GATT discovery/subscription activity, and no panic/watchdog
output. A camera spot-check showed the visible `Synced` message, compact
connection line, four Home tiles, and one `START` action inside the bezel;
touch accuracy and the remaining trainer/HR safety gates still require
hands-on acceptance.

Checkpoint `7e1d439` implements the spec's stop-confirmation flow. The paused
ride action now opens `END WORKOUT?` with separate `END & SAVE` and `CANCEL`
targets; cancel leaves the ride paused and direct finish commands are rejected
unless confirmation is active. The transition is covered through the public
UI snapshot helper, while the ESP presenter renders the dedicated confirmation
screen. Host `make test`, Debug CTest 2/2, strict Clang fallback syntax, and a
clean ESP-IDF build pass. The application image is `0x223a50` bytes with
`0x1dc5b0` free (47%); SHA-256 is
`89828927d90fc320976c12e441f472c59018b4e5c96817c0c54513b0aaf5e3a6`.
Only application offset `0x20000` was written; no bootloader, partition table,
NVS, LittleFS, erase, or credential-bearing backup action was performed. The
bounded monitor identified app `7e1d439`, LittleFS/LVGL readiness, automatic
Wi-Fi recovery, three Intervals HTTP 200 responses, BLE/GATT activity, and no
panic/watchdog output. Physical confirmation of the two-button flow remains
open.

Checkpoint `e3e8d6d` adds a portable, test-first BOOT gesture classifier. GPIO0
input is debounced for 40 ms; a short press emits one event and pauses a
running ride, while the same event maps to the existing Back action elsewhere.
A long press is detected and logged but deliberately has no reset, erase,
recovery, or provisioning meaning. Host smoke/CTest 2/2, strict Clang
fallback syntax, and a clean ESP-IDF build pass. The application image is
`0x223cb0` bytes with `0x1dc350` free (47%); SHA-256 is
`f7bbc469776c07ebb96bbf56715ad44d0f9ed06ce3efdf5e0f2bb3764b1967ca`.
Only application offset `0x20000` was written. A passive bounded monitor
identified app `e3e8d6d`, the expected project partition table, LittleFS/LVGL
readiness, automatic Wi-Fi association and three Intervals HTTP 200 responses,
BLE/GATT setup, and no panic/watchdog output. No bootloader, partition table,
NVS, LittleFS, erase, or credential-bearing backup action was performed.
Physical BOOT short-press, long-press hold, display readability, touch
accuracy, trainer/HR control safety, and endurance gates remain open.

Checkpoint `38ad6f5` adds bounded retry diagnostics: reset reason, runtime
stack high-water mark, internal heap margin, and retry stage boundaries are
logged without including credentials, addresses, or response bodies. The
previous 8192-word runtime task reached only 744 free words after the normal
three-request Intervals sync; that automatic path completed with HTTP 200
responses and no panic or watchdog output, but the margin made an intermittent
manual-retry stack failure a concrete risk.

Checkpoint `9085764` raises only the runtime task stack to 12288 words. The
clean ESP-IDF image is `0x2240a0` bytes with `0x1dbf60` bytes free (46%) and
SHA-256
`d752f8e254820ed829b63613d31d443f875d7273af649b4570e569cb2c8574fa`.
It was flashed only at application offset `0x20000`; esptool verified the
image hash. No bootloader, partition table, NVS, LittleFS, erase, or
credential-bearing backup action was performed. Host smoke tests and Debug
CTest 2/2 pass.

A passive serial window after the write observed the bootloader and no reset
or panic line during the capture. The application log was not replayed because
the reader attached after boot, and a second attached window did not capture a
manual Retry action. Therefore the larger stack is deployed and the automatic
path is previously evidenced, but the reported manual-retry restart is not yet
physically reproduced on this image. The next hardware gate is one attached
serial capture with a single Home Retry tap; do not issue a serial reset.

Checkpoint `b4504d9` fixes the retry-feedback precedence reported during user
verification. While the serialized runtime task is performing a foreground
sync, the visible feedback now says `Retrying connection and sync` even when
Preview also has the independent `Fresh trainer power required` safety gate.
After the operation completes, the trainer-power message returns. A
test-first portable UI contract covers both busy and idle precedence cases.
The clean ESP-IDF image is `0x224190` bytes with `0x1dbe70` bytes free (46%)
and SHA-256
`5524bc56d77011a18dfa41385380b1779450b56c7eaade4673d50a5fb5dac7a3`.
It was flashed only at application offset `0x20000` after manually entering
download mode; esptool verified the image hash. No bootloader, partition table,
NVS, LittleFS, erase, or credential-bearing backup action was performed.
The passive post-flash window contained no reset event; visual confirmation of
the busy message remains the hands-on check.

Checkpoint `6b832c2` removes the synchronous Wi-Fi/SNTP/Intervals sequence
from the provisioned boot path. Storage, display, BLE, and the runtime task
now establish the ready UI first; the existing bounded sync retry performs
network work afterward. The clean ESP-IDF image is `0x223ed0` bytes with
`0x1dc130` bytes free (46%) and SHA-256
`d4992464519e98b948c26cbce8d6791b7a2488cef6fed9af02417834d4b53f9a`.
It was flashed only at application offset `0x20000` after manual download-mode
entry, with esptool hash verification. The boot trace reached the runtime task
and ready path at approximately 2.3 seconds; Wi-Fi and Intervals sync then ran
in the background. No bootloader, partition table, NVS, LittleFS, erase, or
credential-bearing backup action was performed.

That bounded trace also recorded one HTTP 200 response whose transport ended
with incomplete data; the response was treated as a sync failure and the
durable retry policy remained active. No panic or watchdog output appeared.

## Intervals transport and BLE-memory follow-up — 2026-09-18

The complete sanitized trace from checkpoint `6b832c2` distinguishes two
failures. The first sync attempt hit a Wi-Fi association timeout (`reason=2`);
the next attempt associated and acquired an IPv4 lease. The later Intervals
request received HTTP status `200`, then logged `esp-aes: Failed to allocate
memory`, an mbedTLS read error, and `HTTP_CLIENT: Incomplete chunked data
received -1`. No HTTP `429` or `Retry-After` was present, so this trace does
not support API throttling. The failure occurred while reading a chunked TLS
body, before JSON/ZWO parsing; no panic or watchdog reset appeared. The raw
body and credentials were not captured.

Checkpoint `95c62b4` keeps a non-empty 2xx GET body for the existing JSON/ZWO
validators when the transport reports `ESP_ERR_HTTP_INCOMPLETE_DATA`. Empty
bodies and all POST responses remain strict, so a lost upload acknowledgement
cannot become an upload success. Checkpoint `3a8f4b8` defers automatic sync
while scanning or while a selected BLE link is connected but service discovery
is incomplete; manual Retry remains immediate. Retry diagnostics now include
the DMA-capable heap margin used by the TLS/AES path. Host smoke, CTest 2/2,
strict Clang fallback syntax, and a clean ESP-IDF build pass.

The unflashed `3a8f4b8` application is `0x2240d0` bytes with `0x1dbf30`
free (46%); bootloader size is `0x5160`; SHA-256 is
`f4e1273051d30f3954ff58700ed1a03f623aa40639918db67ee0976ff605d7e0`.
The next hardware gate is an authorized application-only write at `0x20000`
followed by one bounded serial capture of BLE settling, Wi-Fi sync, and the
DMA/HTTP result. No bootloader, partition table, NVS, LittleFS, erase, or
credential-bearing backup action is part of this checkpoint.

## Startup BLE-settling follow-up — 2026-09-18

The first hardware capture after the `3a8f4b8` app-only write falsified a
readiness-only gate: automatic sync began at about 2.3 seconds, before the
trainer's GATT discovery began at about 2.5 seconds. The trainer and HR
procedures then occupied the radio through approximately 13.4 seconds, while
Wi-Fi association timed out. No destructive action or reset command was used;
the board was power-cycled and the reconnecting serial reader captured the
application from its re-enumerated port.

Checkpoint `977f434` adds a 20-second startup grace period to automatic sync
requests, including the post-provisioning handoff. The normal Retry command
still requests an immediate attempt. A portable test covers delayed due-time
and preserves the existing backoff behavior. Strict fallback syntax and clean
ESP-IDF validation pass.

The unflashed `977f434` application is `0x224130` bytes with `0x1dbed0` free
(46%); bootloader size is `0x5160`; SHA-256 is
`49de80598f2ac0db26498a7661f68d03f3f916c18c720d7b848d450e202f67fc`.
The next hardware gate is an app-only write at `0x20000`, followed by a
power-cycle capture that waits for the delayed sync and records the new
DMA-capable heap margins and parser result.

## NimBLE PSRAM allocation follow-up — 2026-09-18

The delayed-sync capture confirmed the local failure: after HTTP client
initialization the DMA-capable heap had only 2,176 bytes in its largest block;
AES then failed while copying PSRAM TLS data through DMA scratch memory. The
partial 752-byte JSON body was retained by `95c62b4` and correctly rejected by
the JSON validator, so incomplete-body salvage does not mask truncation.

Checkpoint `f05d994` selects PSRAM for NimBLE host allocations while retaining
the controller and DMA-capable allocations in their required memory. This
frees internal/DMA heap for HTTPS/AES; the 20-second startup delay remains as a
separate transient-overlap guard. The clean image is `0x224130` bytes with
`0x1dbed0` free (46%); bootloader size is `0x5160`; SHA-256 is
`62166f998d455288b703929fe81fd00f81bacb3df544db94a9a5f86e85f3b246`.
Host smoke, CTest 2/2, strict fallback syntax, and ESP-IDF build pass. The
image is not yet flashed; the next gate is one app-only write at `0x20000` and
an attached power-cycle capture with trainer and HR links available.
