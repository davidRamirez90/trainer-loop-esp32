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
external SDK validator). The final laptop-only `idf.py build` also passes: app
`0x21fa90`, free `0x1e0570` (47%) in the current app partition; bootloader
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

Checkpoint `cd70fad` was built and flashed to the application slot only at
`0x20000` (SHA-256
`61fea417e3b87456959f44ebbf158a5d6e9c1c6c0aef44df0618c79e3c74f724`). A
bounded monitor window confirmed app version `cd70fad`, the project partition
table, LittleFS mount, LVGL readiness, Wi-Fi recovery, three Intervals HTTP 200
responses, and no panic/watchdog. The remaining work is physical acceptance of
the updated touch flow, exact trainer/HR pairing, safe ERG acknowledgements,
ride completion/history navigation, and the release-gate endurance and
reconnect tests in the checklist.
