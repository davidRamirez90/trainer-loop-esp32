# Native simulator implementation evidence

The acceptance scope is the complete three-stage simulator described in the
user task. A screen gallery or portable-core tests alone do not close it.

## Baseline (2026-09-30)

- Existing uncommitted core/UI/firmware changes are preserved.
- Native Debug CMake build and CTest: 2/2 passed, including independent Garmin
  SDK FIT integrity and semantic validation.
- ESP-IDF 5.5.4 `idf.py build`: passed; application 0x229100 bytes,
  0x1d6f00 bytes (46%) free. No physical device operation performed.

## Acceptance checklist

- [x] Production LVGL 9.2.2 screens, device fonts/configuration, 368×448.
- [x] Desktop window, headless framebuffer and PNG capture.
- [x] Fixtures for all screens and significant empty/loading/error/safety states.
- [x] Real LVGL press/release/movement, blank and disabled targets, navigation.
- [x] One-command build/run from documented clean checkout dependencies.
- [x] Shared startup, orchestration, guards, runtime, recording and upload policy.
- [x] Deterministic monotonic/UTC advancement processing intermediate ticks/events.
- [x] Trainer/HR discovery, selection, generation/capability/notification handling.
- [x] ACK delay/rejection/timeout, stale data, disconnect/reconnect controls.
- [x] Fixture Wi-Fi/time/HTTP with production sync parsing and retry behavior.
- [x] Isolated retained storage using production journal/cache/history/FIT formats.
- [x] Settings persistence, restart/recovery and defined storage failures.
- [x] Production BOOT gesture handling.
- [x] Serialized machine-readable control protocol with state/controls/traces.
- [x] Scenarios: setup, selection, pairing, start/progression, pause/resume,
  stop/save, discard, completion, history, settings, offline cache, sync/upload
  retry, interrupted recovery.
- [x] Safety: trainer loss/stale power/command rejection/timeout/old generations;
  reconnect never resumes automatically; explicit reacquisition/resume tested.
- [x] Simulator FIT artifacts independently validated with Garmin SDK.
- [x] Reproducible screenshots, reviewed baselines and visual regression checks.
- [x] Failure artifacts: input/seed/time/traces/state/images/storage.
- [x] Single regression command; no existing repository CI to extend.
- [x] Final complete host suite + simulator suite + FIT SDK + ESP build.
- [x] Architecture/protocol/authoring/failure inspection/limitations documented.

## Hardware-only validation

Physical readability and touch feel, actual resistance, radio interoperability,
ESP32 scheduling/memory pressure, and physical flash durability still require
hardware validation. This simulator task does not authorize device operations.

## Integration evidence

- Clean native build compiles production application, synchronization parser,
  LVGL presenter and domain core against LVGL 9.2.2.
- Visual verifier passes all 26 reviewed fixtures, including structured control
  bounds/enabled state and raw enabled/disabled input checks. Baseline review is
  recorded in `host/simulator/tests/baselines/review.json`.
- SDL dummy-driver window creation/render loop smoke passes. Desktop adapter
  routes mouse input and B-key BOOT input through the production paths.
- Core and independent Garmin SDK host validation pass after runtime extraction.
- Native macOS SDL window creation/render/exit also passes without the dummy
  driver (three frames); retained `--provisioned` launch preserves settings.
- Final `make simulator-test`: CTest 5/5 passed in 12.10s, including all 32
  scenarios, 26 reviewed visual/state fixtures, core suite, Garmin SDK validation,
  and SDL window smoke. Log: `simulator-artifacts/final-regression.log`.
- Final `env -u IDF_PATH make test
  HOST_TEST="$PWD/simulator-artifacts/trainer-loop-core-tests"`: standalone core
  executable plus CTest 2/2 passed. The override accommodates this machine's
  non-writable `/tmp`; tests otherwise use the OS temporary directory.
  Log: `simulator-artifacts/host-suite.log`.
- Final ESP-IDF 5.5.4 `idf.py build` passes: application 0x22a960 bytes,
  0x1d56a0 bytes (46%) free. Log: `simulator-artifacts/firmware-build.log`.
- Clean-source reproduction: exported current maintained sources into
  `simulator-artifacts/clean-source` without build directories, managed components
  or the FIT environment. ESP-IDF fetched the declared components; `idf.py
  set-target esp32s3` configured the correct target. Created a new `.venv-fit`,
  installed the pinned requirements, and ran `make simulator-test`: CTest 5/5,
  all 32 scenarios and visual/FIT checks passed in 21.83s. Logs:
  `simulator-artifacts/clean-dependencies.log`, `clean-target.log`,
  `clean-fit-setup.log`, and `clean-regression.log`. This export includes the
  uncommitted implementation being delivered; it is not a checkout of old HEAD.
  Resolution was also verified after removing the ignored local lockfile
  (`clean-unlocked-dependencies.log`); LVGL remains pinned to 9.2.2.
- That reproduction exposed the production 192-byte FIT-path bound. Native
  storage now chooses the shorter canonical absolute/relative spelling without
  changing persisted format limits. Lifecycle/save/FIT validation also passes
  with a symlink-plus-`..` root (`simulator-artifacts/path-check.log`).
- Reviewed shared source boundaries: both targets compile production runtime,
  presenter and Intervals parser; platform adapters contain transport/storage/
  clock replacements, and screen fixtures contain data only. The wait callback
  preserves 1Hz recording and safety checks without recursive control commands.
  Scenarios verify stale-data start/resume aborts, deferred safety pause after an
  outstanding ACK, intermediate loss/reconnect deadlines, completion clock
  freezing, and explicit reacquisition before resume.
- `git diff --check` passes. Existing uncommitted work remains in place.
