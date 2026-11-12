# Include from host/CMakeLists.txt. LVGL comes from the ESP-IDF Component
# Manager's pinned lvgl/lvgl 9.2.2 checkout, so host screenshots exercise the
# same LVGL implementation and production presenter as firmware.
set(LV_CONF_PATH "${CMAKE_SOURCE_DIR}/host/simulator/lv_conf.h" CACHE FILEPATH "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)
set(LV_LVGL_H_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)
set(TRAINER_LVGL_SOURCE "${CMAKE_SOURCE_DIR}/managed_components/lvgl__lvgl" CACHE PATH "Pinned LVGL 9.2.2 source checkout")
file(READ "${TRAINER_LVGL_SOURCE}/lv_version.h" TRAINER_LVGL_VERSION_HEADER)
if(NOT TRAINER_LVGL_VERSION_HEADER MATCHES "LVGL_VERSION_MAJOR 9" OR
   NOT TRAINER_LVGL_VERSION_HEADER MATCHES "LVGL_VERSION_MINOR 2" OR
   NOT TRAINER_LVGL_VERSION_HEADER MATCHES "LVGL_VERSION_PATCH 2")
  message(FATAL_ERROR "Simulator requires the production pinned LVGL 9.2.2")
endif()
add_subdirectory("${TRAINER_LVGL_SOURCE}"
                 "${CMAKE_BINARY_DIR}/lvgl-host" EXCLUDE_FROM_ALL)
add_library(trainer_simulator_lvgl ALIAS lvgl)
find_package(ZLIB REQUIRED)

# SDL is an optional interactive view. Use the Homebrew/system sdl2-config
# interface directly; the screenshot and pointer APIs remain available without
# SDL installed.
find_program(TRAINER_SDL2_CONFIG NAMES sdl2-config PATHS /opt/homebrew/bin /usr/bin)
if(TRAINER_SDL2_CONFIG)
  execute_process(COMMAND "${TRAINER_SDL2_CONFIG}" --cflags
    OUTPUT_VARIABLE TRAINER_SDL2_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${TRAINER_SDL2_CONFIG}" --libs
    OUTPUT_VARIABLE TRAINER_SDL2_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  separate_arguments(TRAINER_SDL2_CFLAGS NATIVE_COMMAND "${TRAINER_SDL2_CFLAGS}")
  separate_arguments(TRAINER_SDL2_LIBS NATIVE_COMMAND "${TRAINER_SDL2_LIBS}")
  set(TRAINER_SDL2_LINK_DIRECTORIES)
  set(TRAINER_SDL2_LINK_LIBRARIES)
  set(TRAINER_SDL2_LINK_OPTIONS)
  foreach(flag IN LISTS TRAINER_SDL2_LIBS)
    if(flag MATCHES "^-L(.+)$")
      list(APPEND TRAINER_SDL2_LINK_DIRECTORIES "${CMAKE_MATCH_1}")
    elseif(flag MATCHES "^-l(.+)$")
      list(APPEND TRAINER_SDL2_LINK_LIBRARIES "${CMAKE_MATCH_1}")
    else()
      list(APPEND TRAINER_SDL2_LINK_OPTIONS "${flag}")
    endif()
  endforeach()
  add_library(trainer_simulator_sdl INTERFACE)
  target_sources(trainer_simulator_sdl INTERFACE "${CMAKE_CURRENT_LIST_DIR}/lvgl_sdl.cpp")
  target_compile_options(trainer_simulator_sdl INTERFACE ${TRAINER_SDL2_CFLAGS})
  target_link_directories(trainer_simulator_sdl INTERFACE ${TRAINER_SDL2_LINK_DIRECTORIES})
  target_link_libraries(trainer_simulator_sdl INTERFACE ${TRAINER_SDL2_LINK_LIBRARIES})
  target_link_options(trainer_simulator_sdl INTERFACE ${TRAINER_SDL2_LINK_OPTIONS})
  target_compile_definitions(trainer_simulator_sdl INTERFACE TRAINER_LOOP_HAS_SDL=1)
endif()
