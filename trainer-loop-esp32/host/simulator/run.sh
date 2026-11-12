#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
# Configure native CMake even in an ESP-IDF-enabled developer shell.
env -u IDF_PATH cmake -S "$repo" -B "$repo/build-simulator" \
  -DCMAKE_BUILD_TYPE=Debug -DTRAINER_LOOP_BUILD_SIMULATOR=ON >&2
cmake --build "$repo/build-simulator" --parallel >&2
exec "$repo/build-simulator/host/simulator/trainer_simulator" "$@"
