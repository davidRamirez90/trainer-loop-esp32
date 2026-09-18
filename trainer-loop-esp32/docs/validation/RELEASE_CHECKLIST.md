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
- [x] Static source audit of the current application image found no
  `ESP_LOG*`, `printf`, or equivalent path that prints those secrets, request
  headers, or HTTP bodies. The unchecked item remains open for physical crash
  output and release-log review.
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

## Latest application-only validation — 2026-09-18

- [x] Checkpoint `0ce346c` was built from a clean Git commit and flashed only
  to application offset `0x20000`; esptool verified SHA-256
  `9e3ac95d1b352287a82045bdcff0eaa626845b2ca243de7698dddf991373f1de`.
  No bootloader, partition table, NVS, or LittleFS storage write was
  performed.
- [x] The image is `0x220b70` bytes with `0x1df490` free (47%). Boot evidence
  confirmed the new app version, project partition table, LittleFS mount,
  SH8601/LVGL startup, Wi-Fi recovery, three Intervals HTTP 200 responses,
  BLE/GATT startup, and no panic/watchdog output.
- [x] History retry is now explicitly labeled `RETRY UPLOAD` and routed to
  the durable upload queue; Home `RETRY` remains workout synchronization.
- [ ] Physically exercise a failed upload and confirm the new History action
  retries it without changing the selected workout. Existing user acceptance
  covers device connection, History navigation, and a static completed-ride
  elapsed timer.

## Latest application-only validation — 2026-09-18

- [x] Checkpoint `11c1ddc` was built from a clean Git commit and flashed only
  to application offset `0x20000`; esptool verified SHA-256
  `e6bc78e18a9009cd9bb7bf180d4e3d147553957deca04ae0b022e596baac7005`.
  No bootloader, partition table, NVS, or LittleFS storage write was
  performed.
- [x] The image is `0x220f70` bytes with `0x1df090` free (47%). Boot evidence
  confirmed app version `11c1ddc`, the project partition table, LittleFS
  mount, SH8601/LVGL startup, Wi-Fi recovery, three Intervals HTTP 200
  responses, BLE/GATT startup, and no panic/watchdog output.
- [x] Host `make test`, Debug CMake build and CTest 2/2 passed, including the
  independent FIT validator. The date-label behavior was added test-first.
- [x] A live camera spot-check showed the inset Home status line, workout
  context and larger action targets on the physical display.
- [ ] Complete direct touch/readability acceptance, including the Home/Preview
  two-line workout label, all four touch corners, pairing scan retry and
history upload retry. Existing hardware ride and upload evidence remains
  valid; this visual spot-check does not close those release gates.

## Latest application-only validation — 2026-09-18

- [x] Checkpoint `796c401` was built from a clean Git commit and flashed only
  to application offset `0x20000`; esptool verified SHA-256
  `123bfb3fca44d4f826f3c21b5cda66aaa5bda8b35e6e47f43cdfa8ce5a1542df`.
  No bootloader, partition table, NVS, or LittleFS storage write was
  performed.
- [x] The image is `0x221380` bytes with `0x1dec80` free (47%). Boot evidence
  confirmed app version `796c401`, LittleFS mount, SH8601/LVGL startup,
  Wi-Fi recovery, BLE/GATT discovery, three Intervals HTTP 200 responses, and
  no panic/watchdog output.
- [x] Host `make test`, Debug CMake build and CTest 2/2 passed. Test-first UI
  coverage verifies normalized interval-profile values.
- [x] A live camera spot-check showed the eight-bar profile above the shifted
  Home/Preview action tiles and no obvious bezel clipping.
- [ ] Complete direct touch/readability acceptance and exercise the profile
  across short, long and multi-workout plans on the physical display.

## Laptop-only confidence follow-up — 2026-09-18

- [x] Long-plan profile mapping is covered by checkpoint `fef531e`; host smoke,
  Debug CMake/CTest, strict Clang `-Werror`, and ASan/UBSan checks pass.
- [ ] Physical touch/readability, BLE reconnect/control acknowledgements, and
  endurance/power-loss gates remain hardware work. The test-only checkpoint was
  not flashed; at that point the device remained on application image
  `796c401`.

## Laptop-only cache consistency follow-up — 2026-09-18

- [x] Checkpoint `6a6bfea` replaces the cached workout set after a successful
  daily sync and clears it for a successful empty response. Failed writes are
  tested to avoid pruning existing same-day records.
- [x] Host tests and clean ESP-IDF build pass; image size is `0x221af0` with
  `0x1de510` free (47%), SHA-256
  `f59073d360236474e7bad3e88fab20a3d2753d426579e4818aafc4d1cfa30e6a`.
- [x] This intermediate cache image was not flashed before the final
  application-only update; its hardware UI, BLE, ride, storage, and service
  gates remain open.

## Latest application-only validation — 2026-09-18

- [x] Final checkpoint `713e2c0` was flashed only to application offset
  `0x20000`; esptool verified the `0x221af0`-byte image with SHA-256
  `b9d9cfadf1965125b6167da67af0cd36ca7e740a1c1cf661e6ad0f8a16af8a0b`.
  No bootloader, partition table, NVS, or LittleFS storage write was made.
- [x] The bounded boot monitor confirmed app version `713e2c0`, the existing
  project partition table, LittleFS/LVGL startup, Wi-Fi recovery, three
  Intervals HTTP 200 responses, BLE/GATT discovery/subscription activity, and
  no panic/watchdog output.
- [ ] Direct touch/readability, cache-replacement behavior on the real account,
  BLE reconnect/control acknowledgements, ride safety, and endurance gates
  remain open. The RTC oscillator-stop warning remains documented.

- [x] Test checkpoint `45c9258` adds invalid-date and empty-directory cache
  replacement boundaries without changing the firmware image.

## Latest application-only validation — 2026-09-18

- [x] Final application checkpoint `23ac626` was built cleanly and flashed only
  to application offset `0x20000`; esptool wrote `0x221b10` bytes and verified
  SHA-256
  `c5aa36238975c471e56a9f715d9ab1814358663fcfb2b52423ac22930aa92159`.
  No bootloader, partition table, NVS, or LittleFS storage write was made.
- [x] The bounded monitor identified app version `23ac626`, the expected
  project partition table, LittleFS/LVGL startup, automatic Wi-Fi recovery and
  BLE/GATT startup with no panic/watchdog output. The serialized reconnect
  path showed no concurrent-connect deferral during the observed startup
  window. App size is `0x221b10`, with `0x1de4f0` free (47%).
- [x] A live camera spot-check showed the compact connection line and four Home
  action tiles inside the display bezel without obvious clipping.
- [ ] Physical acceptance remains open for scan/reconnect behavior, trainer
  and HR pairing by explicit address/type, control acknowledgements, pause and
  stop safety, cache replacement using the real account, touch/readability,
  and endurance. The RTC oscillator-stop warning remains a time-validity gate.

## Latest application-only validation — 2026-09-18

- [x] Application checkpoint `2fba6d3` was built from a clean Git commit and
  flashed only to application offset `0x20000`; esptool wrote `0x2220a0` bytes
  and verified SHA-256
  `b323236c9e996f668a9922e8e9df66a142f1cf1020e79499a9433c598e0bb646`.
  No bootloader, partition table, NVS, or LittleFS storage write was made.
- [x] The image adds deterministic same-day cache ordering, covered by a
  public-interface host test. Boot evidence identified app version `2fba6d3`,
  the expected partition table, LittleFS/LVGL startup, automatic Wi-Fi
  recovery, three Intervals HTTP 200 responses and BLE/GATT setup with no
  panic/watchdog or concurrent-connect deferral in the observed window.
- [ ] Physical acceptance remains open for the real-account cache selection,
  scan/reconnect behavior, explicit trainer/HR pairing, control acknowledgements,
  pause/stop safety, touch/readability, and endurance.

## Latest application-only validation — 2026-09-18

- [x] Application checkpoint `8ac2336` was built from a clean Git commit and
  flashed only to application offset `0x20000`; esptool wrote `0x222cd0` bytes
  and verified SHA-256
  `0475570e50382541d0be13a04a1dad735ea0098e155e842744489b11bdcb2f36`.
  No bootloader, partition table, NVS, or LittleFS storage write was made.
- [x] Cache replacement now restores the prior valid same-date set after a
  partial multi-record write or prune failure. Host storage contracts cover
  this failure boundary. Boot evidence identified app version `8ac2336`, the
  expected partition table, LittleFS/LVGL startup, automatic Wi-Fi recovery,
  three Intervals HTTP 200 responses and BLE/GATT setup with no
  panic/watchdog output.
- [ ] Physical acceptance remains open for real-account cache selection,
  scan/reconnect behavior, explicit trainer/HR pairing, control acknowledgements,
  pause/stop safety, touch/readability, and endurance.

## Latest application-only validation — 2026-09-18

- [x] Checkpoint `c4700b2` adds an explicit waiting state for temporary BLE
  setup conflicts and retries the foreground scan every 250 ms. Back cancels
  the wait; non-conflict errors remain visible.
- [x] Host tests, strict Clang `-Werror`, ASan/UBSan, and a clean ESP-IDF build
  pass. The image is `0x222f80` bytes with `0x1dd080` free (47%).
- [x] The image was flashed only to application offset `0x20000` after the
  final Git checkpoint. Boot evidence confirms app `c4700b2`, the expected
  partition table, LittleFS/LVGL readiness, Wi-Fi lease acquisition, three
  Intervals HTTP 200 responses, BLE/GATT setup, and no panic/watchdog output.
  No bootloader, partition, NVS, LittleFS, erase, or credential backup action
  was performed.
- [ ] Supervised physical Devices-scan, touch/readability, explicit pairing,
  control acknowledgement, pause/stop safety, and endurance gates remain open.

## Latest application-only validation — 2026-09-18

- [x] Pairing candidates now show role/name on line one and the complete
  address plus RSSI on line two, without changing explicit address/type
  selection or BLE policy.
- [x] Host, strict Clang, ASan/UBSan, and ESP-IDF validation pass; the
  application image is `0x222fb0` bytes with `0x1dd050` free (47%).
- [x] Checkpoint `6244923` was flashed only to application offset `0x20000`
  after the final Git checkpoint; esptool verified SHA-256
  `57ab51736f74a41f2f642f8fa25ce7266361e61857b89c51f4d1c2811af183a5`.
  Boot evidence confirms app `6244923`, the expected partition table,
  LittleFS/LVGL readiness, Wi-Fi recovery, three Intervals HTTP 200 responses,
  BLE/GATT setup, and no panic/watchdog output.
- [ ] Perform the physical readability/touch check for the two-line pairing
  rows, then continue explicit pairing, control acknowledgement, pause/stop
  safety, and endurance gates.

## Latest application-only validation — 2026-09-18

- [x] All `main/*.cpp` translation units pass the host-safe Clang `-Werror`
  syntax check; ESP-only startup and scan state no longer trigger unused-variable
  diagnostics in the fallback build.
- [x] `make test` and CTest 2/2 pass after the fix.
- [x] Clean ESP-IDF build produced app `6aba925`, size `0x222fb0` bytes with
  `0x1dd050` free (47%); bootloader size is `0x5160`.
- [x] The verified image SHA-256 is
  `7b6797986fc608bf72f8020223c8cc89969458192d652fc9736c69e372703e03`.
- [x] The image was written only to application offset `0x20000` after the
  final source checkpoint. Bounded boot evidence captured LittleFS/LVGL
  readiness, three Intervals HTTP 200 responses, BLE/GATT setup, and no
  panic/watchdog output.
- [x] No bootloader, partition table, NVS, LittleFS, erase, or
  credential-bearing backup action was performed.
- [ ] Physical readability/touch for the two-line pairing rows, explicit
  pairing, control acknowledgement, pause/stop safety, and endurance gates
  remain open.

## Latest application-only validation — 2026-09-18

- [x] Test-first UI coverage verifies that disconnected links render `OFF`,
  connected links render `OK`, and reconnecting links render `RETRY`.
- [x] `make test`, CTest 2/2, and strict Clang `-Werror` fallback syntax
  validation pass.
- [x] Clean ESP-IDF build produced app `d1e1e64`, size `0x222fa0` bytes with
  `0x1dd060` free (47%); bootloader size is `0x5160`.
- [x] Verified image SHA-256:
  `7427bf1c90a0fe4ba1467fc49fcc8a7fab16344a06ae72fa095a7c7513231688`.
- [x] The image was flashed only to application offset `0x20000`. Bounded
  monitor evidence confirmed the expected partition table, LittleFS/LVGL
  readiness, Wi-Fi recovery, three Intervals HTTP 200 responses, BLE/GATT
  setup, and no panic/watchdog output.
- [x] No bootloader, partition table, NVS, LittleFS, erase, or
  credential-bearing backup action was performed.
- [x] Live camera spot-check showed the active ride layout and large action
  controls inside the bezel; it did not prove small-text readability or touch
  accuracy because the feed was soft.
- [ ] Physically confirm the corrected status labels and two-line pairing
  rows, then continue explicit pairing, control acknowledgement, pause/stop
  safety, and endurance gates.

## Latest application-only validation — 2026-09-18

- [x] Home reports the actual cloud-sync result separately from Wi-Fi link
  state, including `Synced`, `No workout today`, `Sync failed`, `Auth blocked`,
  and `Wi-Fi online`.
- [x] Test-first coverage exercises all sync-state labels.
- [x] `make test`, CTest 2/2, and strict Clang `-Werror` fallback syntax
  validation pass.
- [x] Clean ESP-IDF build produced app `af53660`, size `0x2230b0` bytes with
  `0x1dcf50` free (47%); bootloader size is `0x5160`.
- [x] Verified image SHA-256:
  `8e582c9b7634fd973e3e23c181d4b65dfa19b2ef714adad7da015a1dc5da1ab2`.
- [x] The image was flashed only to application offset `0x20000`. Bounded
  monitor evidence confirmed the expected partition table, LittleFS/LVGL
  readiness, Wi-Fi recovery, three Intervals HTTP 200 responses, BLE/GATT
  setup, and no panic/watchdog output.
- [x] No bootloader, partition table, NVS, LittleFS, erase, or
  credential-bearing backup action was performed.
- [ ] Physically confirm the new sync labels and corrected status labels,
  pairing touch targets, control acknowledgement, pause/stop safety, and
  endurance behavior.

## Camera spot-check — 2026-09-18

- [x] Latest screen shows the compact connection line, workout context, four
  distinct Home tiles, and one `START` action inside the bezel without obvious
  clipping.
- [x] Text is readable enough for layout review; touch accuracy,
  disconnect-state transitions, control acknowledgement, and safety remain
  hands-on gates.

## Latest application-only validation — 2026-09-18

- [x] Checkpoint `718d19e` reports the actual sync outcome in the visible Home
  message line (`Synced` or `No workout today`; detailed errors are retained).
- [x] Host tests, Debug CTest 2/2, and strict Clang `-Werror` validation pass.
- [x] Clean ESP-IDF build produced a `0x223090`-byte application with
  `0x1dcf70` free (47%); bootloader size remains `0x5160`.
- [x] Verified image SHA-256:
  `fc15d9995ed297164b14123609fda55b7d831e74304ca5540b40b0e609960798`.
- [x] The image was flashed only to application offset `0x20000`; esptool
  verified the hash. No bootloader, partition table, NVS, LittleFS, erase, or
  credential-bearing backup action was performed.
- [x] Bounded monitor evidence confirmed app `718d19e`, the expected project
  partition table, LittleFS/LVGL readiness, automatic Wi-Fi recovery, three
  Intervals HTTP 200 responses, BLE/GATT discovery/subscription activity, and
  no panic/watchdog output.
- [x] Camera evidence showed the visible `Synced` result, compact connection
  line, four Home tiles, and one `START` action within the bezel.
- [ ] Physical acceptance remains open for sync/link readability, pairing-row
  touch targets, explicit trainer/HR selection, FTMS capabilities and control
  acknowledgements, pause/stop safety, reconnect behavior, and endurance.

## Latest application-only validation — 2026-09-18

- [x] Checkpoint `7e1d439` implements the spec's dedicated stop-confirmation
  screen with separate `END & SAVE` and `CANCEL` actions. A direct finish is
  rejected until the confirmation state is entered.
- [x] Public UI transition coverage and all host/FIT validation pass; strict
  Clang fallback syntax validation also passes.
- [x] Clean ESP-IDF build produced a `0x223a50`-byte application with
  `0x1dc5b0` free (47%); bootloader size remains `0x5160`.
- [x] Verified image SHA-256:
  `89828927d90fc320976c12e441f472c59018b4e5c96817c0c54513b0aaf5e3a6`.
- [x] The image was flashed only to application offset `0x20000`; esptool
  verified the hash. No bootloader, partition table, NVS, LittleFS, erase, or
  credential-bearing backup action was performed.
- [x] Bounded monitor evidence confirmed app `7e1d439`, the expected project
  partition table, LittleFS/LVGL readiness, automatic Wi-Fi recovery, three
  Intervals HTTP 200 responses, BLE/GATT activity, and no panic/watchdog
  output.
- [ ] Physically verify the confirmation/cancel/end-save touch sequence and
  its paused-ride safety behavior before closing the release gate.

## Latest application-only validation — 2026-09-18

- [x] Checkpoint `e3e8d6d` adds a 40 ms debounced BOOT gesture classifier with
  one-shot short and long events. A short press maps to Pause while Running
  and to the existing Back action otherwise; long press is detected and
  ignored safely without reset, erase, recovery, or provisioning behavior.
- [x] Test-first host coverage exercises bounce rejection, short-press
  release, one-shot delivery, and long-press threshold behavior.
- [x] `env -u IDF_PATH make test`, Debug CTest 2/2, and strict Clang
  `-Werror` fallback syntax validation pass.
- [x] Clean ESP-IDF build produced app `e3e8d6d`, size `0x223cb0` bytes with
  `0x1dc350` free (47%); bootloader size remains `0x5160`.
- [x] Verified image SHA-256:
  `f7bbc469776c07ebb96bbf56715ad44d0f9ed06ce3efdf5e0f2bb3764b1967ca`.
- [x] The image was flashed only to application offset `0x20000`; esptool
  verified the image hash. A passive bounded monitor confirmed the expected
  partition table, LittleFS/LVGL readiness, Wi-Fi association, three
  Intervals HTTP 200 responses, BLE/GATT setup, and no panic/watchdog output.
- [x] No bootloader, partition table, NVS, LittleFS, erase, or
  credential-bearing backup action was performed.
- [ ] Physically verify BOOT short press while riding, safe Back behavior in
  non-ride screens, long-press handling, touch/readability, control
  acknowledgements, pause/stop safety, reconnect behavior, and endurance.

## Retry reset diagnosis — 2026-09-18

- [x] Checkpoint `38ad6f5` added sanitized reset, stack, heap, and retry-stage
  diagnostics. The prior automatic retry reached 744 free words on the
  8192-word runtime stack after three successful Intervals HTTP responses.
- [x] Checkpoint `9085764` increased only the runtime task stack to 12288
  words. Clean ESP-IDF image: `0x2240a0` bytes, `0x1dbf60` free (46%),
  bootloader `0x5160`; SHA-256
  `d752f8e254820ed829b63613d31d443f875d7273af649b4570e569cb2c8574fa`.
- [x] App-only write at `0x20000` completed with esptool hash verification.
  No bootloader, partition table, NVS, LittleFS, erase, or credential-bearing
  backup action was performed.
- [x] Host smoke tests and Debug CTest 2/2 pass.
- [ ] Reproduce one manual Home Retry tap while a serial reader is already
  attached; the first post-write window missed application logs because it
  attached after boot, and the second window contained no tap/reset event.

## Retry feedback usability fix — 2026-09-18

- [x] Checkpoint `b4504d9` makes active sync feedback take precedence over the
  unrelated Preview safety text. During a retry, the display now identifies
  the disabled-button operation as `Retrying connection and sync`; after it
  completes, `Fresh trainer power required` is shown again when appropriate.
- [x] Test-first host coverage verifies busy and idle message precedence.
- [x] Clean ESP-IDF image: `0x224190` bytes, `0x1dbe70` free (46%), bootloader
  `0x5160`; SHA-256
  `5524bc56d77011a18dfa41385380b1779450b56c7eaade4673d50a5fb5dac7a3`.
- [x] App-only write at `0x20000` completed after physical download-mode
  entry, with esptool hash verification. No bootloader, partition table, NVS,
  LittleFS, erase, or credential-bearing backup action was performed.
- [ ] Visually confirm the new busy message during one Retry attempt while the
  trainer is out of range; no reset event was captured in the bounded window.

## Fast-ready boot checkpoint — 2026-09-18

- [x] Checkpoint `6b832c2` defers provisioned Wi-Fi/SNTP/Intervals sync until
  after the ready UI and runtime task are running. The bounded boot trace
  reached the runtime task at approximately 2.3 seconds; network sync then
  continued in the background.
- [x] Clean ESP-IDF image: `0x223ed0` bytes, `0x1dc130` free (46%), bootloader
  `0x5160`; SHA-256
  `d4992464519e98b948c26cbce8d6791b7a2488cef6fed9af02417834d4b53f9a`.
- [x] App-only write at `0x20000` completed after manual download-mode entry,
  with esptool hash verification. No bootloader, partition table, NVS,
  LittleFS, erase, or credential-bearing backup action was performed.
- [x] The trace showed no panic/watchdog output. One later HTTP 200 response
  ended with incomplete transport data and was retained as a sync failure for
  automatic retry.
- [ ] Visually confirm the faster Home/ready transition on the device.

## Intervals transport and BLE-memory follow-up — 2026-09-18

- [x] The sanitized hardware trace recorded HTTP `200` followed by an AES
  allocation failure and incomplete chunked TLS data. No HTTP `429` or
  `Retry-After` was observed, so throttling is not evidenced.
- [x] Non-empty incomplete 2xx GET bodies are now passed to the existing
  structural validators; POST upload acknowledgements remain strict.
- [x] Automatic sync waits for BLE scan/service discovery to settle, and
  manual Retry stays immediate. Host, CTest 2/2, strict fallback syntax, and
  ESP-IDF build pass for `3a8f4b8`.
- [ ] Flash and bounded-monitor `3a8f4b8` at application offset `0x20000`.
- [ ] Confirm a successful today/profile/ZWO sync with trainer and HR links
  available, then repeat the no-trainer retry path without a restart.

## Startup BLE-settling follow-up — 2026-09-18

- [x] Hardware evidence showed the readiness-only sync gate was too early:
  automatic sync began before BLE GATT discovery.
- [x] `977f434` adds a 20-second automatic startup grace period; manual Retry
  remains immediate. Host and ESP-IDF checks pass.
- [ ] Flash `977f434` at application offset `0x20000` and capture the delayed
  sync with DMA-capable heap margins.
