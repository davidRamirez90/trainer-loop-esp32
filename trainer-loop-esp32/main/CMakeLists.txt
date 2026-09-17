idf_component_register(
  SRCS "app_main.cpp" "board_v1.cpp" "firmware_app.cpp" "provisioning.cpp" "settings_nvs.cpp" "storage_mount.cpp" "lvgl_presenter.cpp" "ble_nimble.cpp" "wifi_station.cpp" "time_sync.cpp" "intervals_http.cpp" "intervals_sync.cpp"
  INCLUDE_DIRS "."
  EMBED_FILES "web/setup.html"
  REQUIRES trainer_core esp_event esp_netif esp_wifi esp_timer esp_http_client esp_http_server nvs_flash littlefs bt driver esp_driver_i2c esp_driver_spi esp_lcd lvgl json mbedtls
)
