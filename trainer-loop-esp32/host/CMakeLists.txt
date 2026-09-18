cmake_minimum_required(VERSION 3.16)

add_library(trainer_core STATIC
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/ble_protocol.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/ble_manager.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/clock.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/fit_encoder.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/fit_store.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/erg_policy.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/hash.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/history.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/intervals.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/journal.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/physics.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/recorder.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/retry_policy.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/settings.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/summary.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/sync.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/setup.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/state_machine.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/telemetry.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/time_service.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/upload_queue.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/ui.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/workout.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/src/workout_cache.cpp
)

target_compile_features(trainer_core PUBLIC cxx_std_17)
target_include_directories(trainer_core PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/../components/trainer_core/include
)
target_compile_options(trainer_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(trainer_core_tests tests/test_core.cpp)
target_link_libraries(trainer_core_tests PRIVATE trainer_core)
target_compile_features(trainer_core_tests PRIVATE cxx_std_17)
target_compile_options(trainer_core_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
add_test(NAME trainer_core_tests COMMAND trainer_core_tests)

add_executable(fit_fixtures tests/fit_fixtures.cpp)
target_link_libraries(fit_fixtures PRIVATE trainer_core)
set(FIT_PYTHON "${CMAKE_SOURCE_DIR}/.venv-fit/bin/python" CACHE FILEPATH "Python with garmin-fit-sdk installed")
add_test(NAME independent_fit_validation COMMAND "${FIT_PYTHON}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_fit.py" "$<TARGET_FILE:fit_fixtures>"
  "${CMAKE_BINARY_DIR}/fit-validation")
