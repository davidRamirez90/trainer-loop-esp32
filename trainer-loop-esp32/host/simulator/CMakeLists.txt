set(TRAINER_IDF_SOURCE "$ENV{HOME}/esp/esp-idf" CACHE PATH "ESP-IDF source for pinned cJSON parser")
set(CJSON_SOURCE "${TRAINER_IDF_SOURCE}/components/json/cJSON")
if(NOT EXISTS "${CJSON_SOURCE}/cJSON.c")
  message(FATAL_ERROR "Set TRAINER_IDF_SOURCE to ESP-IDF 5.5.4 (cJSON source required)")
endif()
add_library(simulator_cjson STATIC "${CJSON_SOURCE}/cJSON.c")
target_include_directories(simulator_cjson PUBLIC "${CJSON_SOURCE}")
include(lvgl.cmake)
add_executable(trainer_simulator
  simulator_main.cpp platform_services.cpp lvgl_png.cpp ui_fixtures.cpp
  ../../main/firmware_app.cpp ../../main/lvgl_presenter.cpp
  ../../main/intervals_sync.cpp ../../main/intervals_http.cpp
  ../../main/ble_nimble.cpp ../../main/board_v1.cpp
  ../../main/wifi_station.cpp ../../main/time_sync.cpp
  ../../main/settings_nvs.cpp ../../main/storage_mount.cpp
  ../../main/provisioning.cpp)
target_include_directories(trainer_simulator PRIVATE . .. ../../main)
target_compile_definitions(trainer_simulator PRIVATE TRAINER_LOOP_HOST_SIM=1)
target_compile_features(trainer_simulator PRIVATE cxx_std_17)
target_link_libraries(trainer_simulator PRIVATE trainer_core simulator_cjson trainer_simulator_lvgl ZLIB::ZLIB)
if(TARGET trainer_simulator_sdl)
  target_link_libraries(trainer_simulator PRIVATE trainer_simulator_sdl)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME simulator_scenarios COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios.py"
  --binary "$<TARGET_FILE:trainer_simulator>"
  --artifacts "${CMAKE_BINARY_DIR}/simulator-artifacts/scenarios")
add_test(NAME simulator_visual_regression COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/visual_regression.py"
  "$<TARGET_FILE:trainer_simulator>"
  "${CMAKE_BINARY_DIR}/simulator-artifacts/visual")
set_tests_properties(simulator_scenarios simulator_visual_regression PROPERTIES TIMEOUT 120)
set_tests_properties(simulator_scenarios PROPERTIES ENVIRONMENT "TRAINER_FIT_PYTHON=${FIT_PYTHON}")
if(TARGET trainer_simulator_sdl)
  add_test(NAME simulator_sdl_window COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=dummy
    "$<TARGET_FILE:trainer_simulator>" --storage "${CMAKE_BINARY_DIR}/simulator-artifacts/window"
    --window --window-frames 3)
  set_tests_properties(simulator_sdl_window PROPERTIES TIMEOUT 15)
endif()
