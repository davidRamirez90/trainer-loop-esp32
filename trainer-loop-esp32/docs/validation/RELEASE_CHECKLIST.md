# TrainerLoop release checklist

This checklist is the evidence log for phases 2–7 of
[`ESP32_MIGRATION_SPEC.md`](../ESP32_MIGRATION_SPEC.md). An unchecked item is
an open gate; host tests do not substitute for physical, radio, storage,
security, or service validation.

## Latest laptop-only evidence — 2026-09-17

- [x] `make test` and explicit Debug CMake/build/CTest: 2/2 pass, including
  official Garmin FIT Python SDK 21.214.0 validation of four session fixtures.
- [x] ESP-IDF build with runtime settings/retry, pairing-state handling,
  post-ride UI updates, clarified sync errors, multiple-workout selection and
  active interval progress: app `0x220a50`, app partition free `0x1df5b0`
  (47%);
  bootloader `0x5160` with 36% free.
- [x] Deterministic parser, clock, safety/replay, offline eligibility, upload
  failure/reboot, Retry-After rebasing, finalized-session enumeration,
  guarded-format policy, torn chunk, short-write, cache preservation and FIT
  checks.
- [x] Source review: bounded settings/requests, write-only API credentials,
  TLS certificate bundle and disabled HTTP redirects; explicit development
  plaintext NVS label. No irreversible security settings changed.
- [ ] Physically verify the new settings screen, persistence, pause/stop UI,
  AP expiry shutdown and security controls. Clean boot, mount and protected
  setup AP were verified after the authorized replacement; settings and ride
  behavior remain open.
- [x] Resolve firmware replacement compatibility: the live partition sector was
  re-read, secure boot/encryption were confirmed disabled, and the user decided
  development NVS/storage data may be discarded. No credential-bearing backup
  was made; full erase plus manifest replacement was authorized and completed.
- [x] Guard the development LittleFS format-on-mount-failure path: formatting is
  now enabled only after the labeled partition is verified entirely erased;
  existing bytes force a non-destructive recovery-preserving mount failure.
  Host policy tests cover the decision; physical damaged-volume durability and
  recovery remain release gates.

See [Laptop validation and home sequence](LAPTOP_VALIDATION.md) for exact
coverage and limitations. The user's prior evidence confirms Wi-Fi/SNTP,
athlete validation HTTP 200 and settings surviving reboot; it does not close
trainer/HR, upload, encryption or mounted touch acceptance gates.

User-reported hardware follow-up (not laptop evidence): the Garmin HR monitor
appears in scanning and can connect, but no HR values are visible on the device
screen; the trainer does not appear in scanning. Pairing, telemetry display,
and trainer discovery remain open Phase 4/5 gates.

## Earlier bring-up build evidence — 2026-09-17 (before this continuation)

- Native AppleClang CMake/CTest passed: 1/1 test.
- `make test`, strict Clang/GCC host builds, supported macOS ASan/UBSan, and
  host-safe `main/*.cpp` compilation with Clang `-Werror` passed.
- ESP-IDF v5.5.4 `esp32s3` `idf.py build`, `idf.py size`, and
  `idf.py size-components` passed with the pinned Espressif SH8601 2.0.1 and
  LVGL 9.2.2 plus LittleFS 1.20.4 components, active 8 MB octal PSRAM at 80 MHz, and the native
  V1 LVGL display/touch adapter, integrated setup/cache/time path, bounded
  NimBLE FTMS/HRS central transport, and the coordinator-backed runtime
  notification/session path, journal UTC-anchor recovery, FIT/history/upload
  queue services, and a bounded offline recent-history screen. The app image
  is `0x216f40` bytes with `0x1e90c0` bytes (48%) free in the smallest
  current app partition; the
  bootloader is `0x5160` bytes with 36% free. The storage partition is
  `littlefs` at `0x820000`, size `0x6F0000`.
- Authorized serial bring-up on `/dev/cu.usbmodem1101` verified normal
  `SPI_FAST_FLASH_BOOT`, 16 MB flash, 8 MB PSRAM, first-boot LittleFS
  formatting, journal/cache directory creation, SH8601 panel creation, LVGL
  readiness, BLE initialization, and setup AP startup. No panic or watchdog
  reset appeared during the observation window. FT3168 probe returned
  `ESP_ERR_NOT_SUPPORTED`; PCF85063 reported its oscillator-stop flag.
- The attached board was previously audited with `esptool`/`espefuse` in the
  ROM loader. Firmware flashing was separately authorized for this bring-up;
  no full-chip erase, eFuse write, or JTAG halt was performed.

## Host-only checks

- [x] Compile and run the portable core with `make test`.
- [x] Compile and run the same suite with Clang and GCC using `-Werror`.
- [x] Run the core suite under AddressSanitizer and UndefinedBehaviorSanitizer.
- [x] Compile the ESP-IDF shell translation units in their host-safe fallback
  mode with Clang and `-Werror`.
- [x] Exercise bounded FTMS/HRS parsing, malformed ZWO rejection, target
  acknowledgement generations, journal CRC/torn-tail recovery, cache
  integrity, FIT CRC, and upload retry persistence.
- [x] Run the native CMake/CTest build with the installed host toolchain.
- [x] Run an ESP-IDF 5.5.4 build with the project's declared dependency policy,
  including the pinned native SH8601 component and explicit V1 PSRAM mode.

## Phase 1 — preservation and V1 hardware evidence

- [x] Record the non-destructive USB enumeration in
  [`PHASE-1-EVIDENCE.md`](../hardware/PHASE-1-EVIDENCE.md).
- [x] Keep unapproved flashing, erasure, eFuse changes, and JTAG halt out of
  the implementation workflow. Firmware flashes and serial-reset probing were
  performed only after explicit bring-up authorization; no full-chip erase or
  eFuse write was performed.
- [ ] Photograph the rear label and verify the physical V1 revision.
- [x] Record chip revision, JEDEC flash, PSRAM, security state, and the live
  partition table through an explicitly authorized maintenance inspection.
- [ ] Verify TF-card presence/free space, battery, and RTC-backup state.
- [x] Audit the expander address/output mapping, PMU/RTC addresses, reset/enable
  sequencing, and SH8601/FT3168 driver path against the V1 schematic/reference.
  Physical rail behavior and acceptance measurements remain open.

## Phase 2 — platform foundation

- [x] Boot the audited V1 board adapter over its native USB serial link. The
  serial log showed normal `SPI_FAST_FLASH_BOOT`, application entry, and no
  boot-loop during the observation window. Standalone boot without a host and
  the physical power-button path remain open.
- [ ] Verify portrait SH8601 output, FT3168 touch corners, BOOT/PWR behavior,
  RTC validity, watchdog progress, and measured heap budgets.
- [x] Verify the FT3168 frame path on the attached unit with live press/release
  transitions and in-bounds coordinates at the center and multiple touch
  corners/edges. Repeat the complete four-corner usability test before release.
- [x] Mount the selected internal LittleFS with a guarded first-boot format
  only for an entirely erased partition; serial bring-up verified that an
  unformatted partition was initialized and journal/cache directory creation
  succeeded. Existing-volume mount failure remains non-destructive.
- [ ] Verify dual-manifest recovery after interrupted writes and measure
  recovery/write durability on the audited V1 storage configuration.
- [x] Confirm the current partition sizes and application image fit within the
  selected IDF configuration; production sizing remains a release gate.

## Phase 3 — setup and cache

- [ ] From a clean device, join the temporary protected AP and complete the
  browser setup form.
- [ ] Verify expiry, physical unlock, Host/Origin/CSRF checks, bounded body
  handling, write-only secrets, and no secret leakage in logs or URLs.
- [ ] Verify NVS encryption policy and recovery procedure before enabling any
  irreversible secure-boot or flash-encryption configuration.
- [ ] With an authorized account, validate athlete/profile/today-event/ZWO
  payloads, multiple-event selection, wrong-sport rejection, date/DST
  behavior, and cache preservation on failed sync.
- [ ] Power-cycle with Wi-Fi unavailable and start a previously cached workout.

## Phase 4 — BLE and safe ERG

- [x] Implement the ESP-IDF NimBLE central boundary with bounded scan
  candidates, explicit address/type selection, two role links, reconnect
  generations, FTMS/HRS discovery, feature/range reads, required CCCD
  subscriptions, bounded notification delivery, and indication-only control
  acknowledgement. Persisted selections restore by address/type, and the
  runtime consumes notifications into the safety coordinator. No
  auto-selection or auto-resume is permitted.
- [ ] Pair the exact Zwift Hub One by address/type and the exact Garmin strap;
  never auto-select by name or strongest RSSI.
- [ ] Verify all FTMS parser flag combinations and HRS 8/16-bit/contact cases
  against captured packets.
- [ ] Verify feature/power-range negotiation, indication CCCD, one pending
  command, NACK/late/wrong-generation handling, and no ATT-write-as-success.
- [ ] Run conservative ERG steps/ramps and record command/ack latency.
- [ ] Complete at least 20 supervised BLE loss/reconnect cycles. Every
  reconnect must remain paused until fresh power and explicit acknowledged
  control reacquisition.
- [ ] Measure the Hub's last-target hold and pause/stop behavior for BLE loss,
  missing notifications, and device power loss. Approve residual risk before
  release; do not infer a fail-safe from this software.

## Phase 5 — workout, simulation, recording

- [x] Wire the coordinator to the ESP runtime task: generation-filtered FTMS/
  HRS notifications update freshness/link state, cached plans enter Preview,
  Start/Resume remain guarded by trusted time, capabilities, fresh power, and
  storage reserve, and a recovered current journal is preserved as an
  interrupted session before Ready.
- [ ] Compare normalized ZWO step/ramp/IntervalsT vectors at segment
  boundaries and throughout ramps.
- [ ] Verify actual measured power drives speed/distance/elevation while the
  target path remains independent of simulation and synthetic grade.
- [ ] Verify wall, active, and plan clocks across pause/resume and a monotonic
  clock correction.
- [ ] Complete a supervised ride of at least 60 minutes with Hub and Garmin.
- [ ] Interrupt power during sample append and manifest commit; recover the
  valid prefix, retain the interrupted marker, and measure data loss against
  the five-second target.
- [ ] Verify storage-full and write-failure behavior pauses safely and never
  deletes unsynced data.

## Phase 6 — FIT, history, upload

- [x] Integrate durable journal reads with FIT publication, local summaries,
  SHA-256 artifact verification, FIT-pending reconciliation, and an
  outside-the-ride upload worker with persisted retry/auth/permanent states,
  plus a bounded offline recent-history screen. Recovered current journals are
  published to FIT/history/upload before their directory is finalized.
  Accepted JSON responses retain a bounded remote activity ID.
- [x] Independently decode generated complete/partial/paused/recovered FIT
  fixtures with Garmin Python SDK 21.214.0: CRC, UTC, timer events, totals,
  invalid-versus-zero values and descent. Physical ride-file/service acceptance
  remains open; fixtures are not real trainer captures.
- [x] Verify deterministic FIT bytes/SHA-256 across repeated fixture generation
  and stable retry identities in durable-queue tests.
- [ ] Browse complete and partial sessions after reboot with no Wi-Fi.
- [ ] Exercise authorized profile/today/ZWO/upload calls, Unicode and escaped
  names, wrong sport, malformed content, and exact remote activity IDs.
- [ ] Exercise 401/403, 429, 408/5xx, DNS/TLS failure, response loss after an
  accepted upload, reboot while `UPLOADING`, and explicit athlete reassignment.
- [x] Ordinary 422 fixtures, including “duplicate field” and “not a duplicate
  activity”, remain permanent errors. Only the exact development fixture
  “duplicate activity” is recognized; live service wording is not verified.

## Phase 7 — release hardening

- [ ] Complete 90-minute, 120-minute, and four-hour soak runs with reconnect
  and offline cases; record watchdog, heap, command latency, and journal
  throughput measurements.
- [ ] Validate at least five retained 120-minute sessions without TF storage,
  honoring the emergency reserve and protecting unsynced rides.
- [ ] Test corrupted chunks/manifests, fragmented/full volumes, TF removal,
  RTC invalidity, Berlin DST transitions, midnight cache refresh, and time
  changes during a ride.
- [ ] Perform mounted-device touch/usability tests with intended lighting,
  gloves/sweat conditions, and no accidental reset or factory-reset gesture.
- [ ] Review all logs/crash paths for SSIDs, passwords, API keys,
  Authorization headers, raw HTTP bodies, and setup tokens.
- [ ] Approve the documented secure provisioning/recovery process before
  enabling secure boot, flash encryption, or debug lockdown.
- [ ] Record compatible Hub/strap firmware, IDF, board-driver, and encoder
  versions alongside every acceptance result.

## Not part of this release gate

P1 richer controls/library/export/CPS/ramp-test work and separately approved
GPX/free-ride/coaching features remain out of P0. They must not add a trainer
control path or weaken the safety/recovery guarantees above.

## Development replacement follow-up — 2026-09-18

The authorized development replacement completed after a fresh 16 MiB
flash/security check and partition-sector read. Existing data was intentionally
discarded without a credential-bearing backup. Full erase and manifest write
completed; bootloader, application and partition table verified. The first boot
populated OTA metadata as expected. Boot evidence confirms clean LittleFS
initialization, SH8601/LVGL readiness, protected setup AP startup and no
panic/watchdog in the bounded monitor window. See
[maintenance evidence](../hardware/MAINTENANCE-2026-09-17.md).

The board is unprovisioned, so BLE did not initialize. Private web setup is
required before trainer/HR scanning. Do not begin a resistance workout until
FTMS capabilities, telemetry, acknowledgements and safe pause/stop behavior are
validated.

## Latest application-only validation — 2026-09-18

- [x] Checkpoint `79f587b` was flashed only to application offset `0x20000`;
  esptool verified SHA-256
  `144ed3dcd6e9e3b9cc50d4ff841d4a7cd3fe41ae2753401e4070bf56a9a151e1`. No
  bootloader, partition table, NVS, or LittleFS storage write was performed.
- [x] Boot log identified app version `79f587b`, the expected project partition
  table, LittleFS mount, SH8601/LVGL readiness, Wi-Fi recovery, and three
  Intervals HTTP 200 responses without panic/watchdog output. App size is
  `0x220a50`, with `0x1df5b0` free (47%).
- [x] User acceptance confirms device connection, History navigation, and a
  static completed-ride elapsed timer.
- [ ] On-device acceptance of interval-progress and multiple-workout views
  remains open. Confirm that the pairing conflict message can be retried after
  automatic GATT setup settles and that NEXT cycles today's cached workouts.
