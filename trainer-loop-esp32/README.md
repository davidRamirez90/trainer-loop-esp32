# TrainerLoop ESP32

Standalone TrainerLoop firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.8
V1 (SH8601 + FT3168). The project follows
[`docs/ESP32_MIGRATION_SPEC.md`](docs/ESP32_MIGRATION_SPEC.md) and is split
between a portable domain core and an ESP-IDF application shell.

## Current implementation state

The portable core implements the safety-sensitive contracts that can be
verified without hardware: strict ZWO normalization, FTMS/HRS decoding and
command framing, acknowledged-command generations, three-clock timing,
synthetic terrain and virtual speed, 48-byte telemetry samples, CRC journal
recovery, settings validation, setup transactions, FIT generation, and a
durable upload queue. The ESP-IDF shell owns the board boundary and drives
only source-audited V1 expander outputs; unresolved nets remain untouched.

See [`docs/IMPLEMENTATION_STATUS.md`](docs/IMPLEMENTATION_STATUS.md) for the
phase gates and [`docs/hardware/PHASE-1-EVIDENCE.md`](docs/hardware/PHASE-1-EVIDENCE.md)
for the non-destructive hardware evidence collected during this migration.

## Host verification

The host build uses CMake, a C++17 compiler, and the official Garmin FIT Python
SDK for independent FIT validation. In a shell without `IDF_PATH` set:

```sh
python3 -m venv .venv-fit
.venv-fit/bin/pip install -r host/tests/fit-requirements.txt
make test
```

The equivalent explicit native build is:

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

When `IDF_PATH` points to ESP-IDF 5.5, the same tree is consumed by `idf.py`.
Building does not change device state; firmware flashing, filesystem
initialization, and serial-reset bring-up are separate authorized operations.

## Safety boundary

Loss of BLE communication can prevent the device from decreasing trainer
resistance. The firmware therefore pauses locally, invalidates control, shows
“STOP PEDALING — resistance unknown”, and requires an explicit acknowledged
reacquisition before resuming. It does not claim automatic resistance release
until the exact Zwift Hub One behavior has been measured and accepted.

Laptop evidence and the deferred home test sequence are in
[`docs/validation/LAPTOP_VALIDATION.md`](docs/validation/LAPTOP_VALIDATION.md).

## Native simulator

The native simulator compiles the production application and LVGL interface
with local platform fixtures. Build/run with `make simulator`; use the
headless JSON protocol through `host/simulator/run.sh`. Architecture,
dependencies and control commands are in
[the simulator guide](docs/simulator/README.md). `make simulator-test` runs the
simulator-enabled regression build.
