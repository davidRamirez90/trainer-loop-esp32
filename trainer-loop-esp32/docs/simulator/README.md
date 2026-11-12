# Native TrainerLoop simulator

This executable compiles the production `FirmwareApp`, `LvglPresenter`,
Intervals synchronization parser, and portable trainer core. It does not run
an ESP32 binary. Host adapters replace the board, NimBLE transport, Wi-Fi,
SNTP, HTTP, NVS and mount boundary; the application owns screens, command
guards, workout execution, safety, recording, recovery and upload decisions.

## Dependencies and build

Use CMake, a C/C++17 compiler, zlib, ESP-IDF 5.5.4 sources (for its cJSON
parser), and the pinned managed LVGL 9.2.2 component. The default IDF source
location is `$HOME/esp/esp-idf`; set CMake `TRAINER_IDF_SOURCE` for another
location. SDL2 and `sdl2-config` are required for the desktop window; headless
tests can run without SDL2. For example, macOS Homebrew provides `cmake`,
`sdl2`, and `zlib`. On a clean checkout with ESP-IDF installed:

```sh
. "$HOME/esp/esp-idf/export.sh"
idf.py set-target esp32s3
python3 -m venv .venv-fit
.venv-fit/bin/pip install -r host/tests/fit-requirements.txt
make simulator-test
make simulator
```

`set-target` configures local build files and fetches the components declared in
`main/idf_component.yml`; it does not operate a device. The scripts select native
CMake even in an IDF-enabled shell. `make simulator-test` builds and runs the core
suite, independent FIT validator, all application scenarios, visual regression,
and (when SDL2 is installed) the window smoke test. The Garmin SDK dependency is
pinned to 21.214.0. `make simulator` opens the real application using synthetic
credentials and a fresh per-process run directory.

For dependencies installed elsewhere, configure once before using those commands:

```sh
env -u IDF_PATH cmake -S . -B build-simulator \
  -DTRAINER_LOOP_BUILD_SIMULATOR=ON \
  -DTRAINER_IDF_SOURCE=/path/to/esp-idf \
  -DTRAINER_LVGL_SOURCE=/path/to/lvgl-9.2.2 \
  -DFIT_PYTHON=/path/to/python-with-garmin-sdk
```

Run the headless control process:

```sh
host/simulator/run.sh --storage simulator-artifacts/manual-run
```

Build logs go to stderr; stdout is reserved for the protocol. Omit `--storage`
to use a dedicated temporary run directory. Existing storage in a supplied
run directory is retained at launch. The `reset` command clears only its
`storage` subdirectory; `restart` retains it. Synthetic fixtures contact no
live radios, Wi-Fi networks or Intervals accounts.

## Control protocol

Commands are newline-delimited JSON objects on stdin. Each response is one
JSON object on stdout with `ok`, `state`, and optional `error`; an `id` is
returned unchanged when supplied. Commands run serially on the application
loop. No focus or wall-clock sleeps are needed.

```json
{"id":1,"op":"state"}
{"op":"setup"}
{"op":"advance","ms":1000}
{"op":"click","text":"DEVICES"}
{"op":"touch","x":30,"y":400,"pressed":true}
{"op":"touch","x":30,"y":400,"pressed":false}
{"op":"boot","pressed":true}
{"op":"advance","ms":100}
{"op":"boot","pressed":false}
{"op":"advance","ms":100}
{"op":"screenshot","path":"simulator-artifacts/home.png"}
{"op":"restart"}
```

`click` finds the named visible control's center and sends pointer press and
release through LVGL. It does not invoke a `UiCommand` or coordinator method.
`touch` also supports movement while pressed. Disabled controls produce a clear
error when addressed by `click`; raw touch can test that they emit no action.
`advance` processes intermediate 100 ms application iterations, not just the
end timestamp (maximum 24 hours per command). FTMS requests can advance the
controlled clock while awaiting ACKs; returned `now_ms` includes that wait and
can exceed the requested advance deadline. Intermediate waits continue polling
telemetry, applying scheduled events, recording, and detecting safety faults.
They suppress recursive control requests until the outstanding transaction ends.

The returned state includes screen and ride state, clocks, workout/interval,
power target, safety/fault state, available controls with text/bounds/enabled,
and history. It also includes settings, session UUID, sample count and the last
20 persisted/buffered samples, connection generations, fixed seed 0, UTC, and
the last 100 application/platform events. Per-run `commands.jsonl` records the
command and response stream; `events.jsonl` and `platform-events.jsonl` retain
the complete event streams across restarts. Numeric input is range checked;
invalid operations, roles, coordinates, payloads, and disabled controls return
`ok:false` with an error and the current state.

Fault controls:

```json
{"op":"network","wifi":false,"http":false}
{"op":"network","time_sync":false}
{"op":"http","method":"GET","path":"/endpoint","status":503,"body":"","retry_after_s":5}
{"op":"ftms","delay_ms":40,"reject":true}
{"op":"ftms","timeout":true}
{"op":"ftms","disconnect":true,"auto_reconnect":false}
{"op":"ftms","stale_generation":true}
{"op":"storage","write_budget":0}
{"op":"storage","clear":true}
```

Settings/setup values default to synthetic credentials. `setup` accepts `ssid`,
`password`, `athlete_id` and `api_key` for local fixture tests. Storage write
budgets inject production host storage short writes/ENOSPC; they do not compile
into device firmware.

`reset` restores the initial UTC, service defaults, capabilities and fixtures,
and clears run storage. `restart` resets monotonic time to zero, retains UTC and
service fault settings, and starts production recovery against retained storage.
Both clear scheduled events and release BOOT. Host settings use the production
schema, CRC, typed namespace keys and paired commit slots in local files; the
ESP NVS flash implementation itself is replaced at the storage boundary.
The host chooses the shorter canonical absolute or current-directory-relative
storage spelling so nested test exports fit the production history/upload path
fields. Keep the working directory consistent when reusing relative paths;
extremely long roots can still exceed the production 192-byte FIT-path limit.

## Hardware validation boundary

Physical readability, touch feel, actual resistance behavior, radio
interoperability, ESP32 scheduling and memory pressure, and physical flash
durability require hardware validation. Simulator evidence cannot close those
gates. This workflow does not flash, erase, reset or operate physical devices.

## Desktop and fixture gallery

With SDL2 installed (`sdl2-config` on PATH), `make simulator` opens a window
and provisions synthetic local credentials. Mouse input feeds the real LVGL
pointer reader. Hold/release **B** to simulate BOOT; the production debounce and
short/long gesture policy applies. The headless protocol remains the autonomous
test interface.

`{"op":"fixtures"}` lists the immutable visual fixture inputs. Render one
with `{"op":"fixture","name":"active"}` and capture it with `screenshot`.
These snapshots exercise production screens; use `reset` afterward to return to
normal application orchestration. Gallery fixtures are for visual states, while
end-to-end scenarios change state through real controls.

Additional injection commands:

```json
{"op":"device","role":"trainer","available":false}
{"op":"device","role":"trainer","telemetry":false}
{"op":"device","role":"heart_rate","available":true,"telemetry":true}
{"op":"notification","role":"trainer","kind":"bike","generation":0,"hex":"40000000c800"}
{"op":"capabilities","min_watts":50,"max_watts":1000,"increment_watts":1,"control_point":true}
{"op":"clock","utc_ms":1790762400000}
{"op":"clock","utc_ms":1790762400000,"trusted":false}
{"op":"pointer_click","x":180,"y":380}
{"op":"schedule","at_ms":2000,"kind":"device","role":"trainer","available":false}
{"op":"schedule","at_ms":2500,"kind":"network","wifi":false,"http":false}
```

Raw notifications use `bike`, `heart_rate`, `control` or `status` and a bounded
hex payload of at most 64 bytes. `generation` is explicit so obsolete packets
can be tested. Optional `timestamp_ms` defaults to the controlled monotonic
clock. Capability changes apply on connection/service discovery. State includes
connection generations and bounded application/platform traces.
Scheduled events use absolute monotonic milliseconds for the current boot,
preserve insertion order at equal timestamps, and are processed at their actual
deadline even inside a delayed ACK wait. The queue is bounded to 32 events.
`click` also accepts a returned `control_id`; `id` is reserved for response
correlation. `device` and `network` commands default omitted availability flags
to true; FTMS fault flags default to false and delay defaults to 40 ms.

## Scenario authoring and failure inspection

Scenario JSON files live in `host/simulator/tests/scenarios`. Each step contains
`command` plus optional structured `expect` assertions or an expected `error`.
Use `click`, `touch` or `pointer_click` for UI actions. Do not call coordinator
methods directly in UI scenarios. Fixed initial UTC and deterministic peripheral
telemetry make the command stream replayable. Keep each scenario's storage
isolated; use `restart` to test retained settings, cache and interrupted rides.

The scenario runner writes the input/response stream and preserves storage on
failure, plus a failure screenshot. Inspect the fault/safety state and traces,
then examine the production journal, FIT, history and upload queue in the
artifact's storage directory. Generated FIT files are validated using the same
independent Garmin SDK environment as the existing host tests.
CTest passes `FIT_PYTHON` to the scenario validator. For a standalone scenario
run, set `TRAINER_FIT_PYTHON` if the SDK is not in `.venv-fit`:

```sh
python3 host/simulator/tests/scenarios.py \
  --binary build-simulator/host/simulator/trainer_simulator \
  --artifacts simulator-artifacts/replay trainer_loss
```

Assertions support dotted state paths, equality, numeric bounds, collection
membership/length, ordered trace fragments, and sample timing bounds. A failure
keeps `failure.json` with the scenario, commands, responses, stderr and exit code,
`failure.png` when the process remains responsive, and the complete run directory.
Normal CTest artifacts are under `build-simulator/simulator-artifacts`.

Capture visual candidates without updating reviewed baselines:

```sh
python3 host/simulator/tests/visual_regression.py \
  build-simulator/host/simulator/trainer_simulator simulator-artifacts/visual-candidates --capture
```

Review every image together with its structured state and control bounds before
copying candidates to `host/simulator/tests/baselines`. Record the review in
`review.json`. The normal regression compares PNG bytes and the structured
manifest; mismatches keep candidate screenshots and command traces. Do not
blindly regenerate baselines to make a failing check pass.

There is no existing repository CI configuration to extend. The same
`make simulator-test` entry point can be run by a CI job with the documented
native, LVGL, cJSON and Garmin SDK dependencies.

Verification evidence and the completed acceptance checklist are in
[IMPLEMENTATION_CHECKLIST.md](IMPLEMENTATION_CHECKLIST.md).
