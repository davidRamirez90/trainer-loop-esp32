#include "wifi_station.h"

#ifdef ESP_PLATFORM

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace trainer_loop {

bool WifiStation::connected() const { return connected_; }

WifiStation::~WifiStation() {
  if (!event_handlers_registered_) return;
  (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::event_handler);
  (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::event_handler);
}

void WifiStation::event_handler(void* argument, esp_event_base_t event_base,
                                std::int32_t event_id, void* event_data) {
  auto* station = static_cast<WifiStation*>(argument);
  if (station == nullptr) return;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    station->connected_ = false;
    const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(event_data);
    ESP_LOGW("trainer_wifi", "STA disconnected during setup/runtime (reason=%d)",
             event == nullptr ? 0 : event->reason);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    station->connected_ = true;
    ESP_LOGI("trainer_wifi", "STA acquired an IPv4 lease");
  }
}

Status WifiStation::initialize() {
  if (initialized_) return Status::ok();
  esp_err_t error = esp_netif_init();
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return Status::failure(ErrorCode::kTransport, "Wi-Fi netif init failed");
  error = esp_event_loop_create_default();
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return Status::failure(ErrorCode::kTransport, "Wi-Fi event loop init failed");
  if (esp_netif_create_default_wifi_sta() == nullptr) {
    return Status::failure(ErrorCode::kTransport, "Wi-Fi station netif creation failed");
  }
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  error = esp_wifi_init(&config);
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return Status::failure(ErrorCode::kTransport, "Wi-Fi init failed");
  // Keep the station awake while BLE links and HTTPS traffic share the radio.
  // Modem sleep is known to leave some APs in a beacon-timeout/null-frame
  // state; this device is USB-powered and can trade that power saving for a
  // reliable control-plane connection.
  error = esp_wifi_set_ps(WIFI_PS_NONE);
  if (error != ESP_OK) return Status::failure(ErrorCode::kTransport, "Wi-Fi power-save configuration failed");
  error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::event_handler, this);
  if (error != ESP_OK) return Status::failure(ErrorCode::kTransport, "Wi-Fi event registration failed");
  error = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::event_handler, this);
  if (error != ESP_OK) {
    (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::event_handler);
    return Status::failure(ErrorCode::kTransport, "IP event registration failed");
  }
  event_handlers_registered_ = true;
  initialized_ = true;
  return Status::ok();
}

Status WifiStation::connect(const std::string& ssid, const std::string& password) {
  if (!initialized_) return Status::failure(ErrorCode::kConflict, "Wi-Fi is not initialized");
  if (ssid.empty() || ssid.size() > 32U || password.size() > 64U) return Status::failure(ErrorCode::kBounds, "Wi-Fi credentials are invalid");
  wifi_config_t config{};
  std::memcpy(config.sta.ssid, ssid.data(), ssid.size());
  std::memcpy(config.sta.password, password.data(), password.size());
  config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  // Runtime Wi-Fi is STA-only. AP+STA exists only in the separately owned
  // provisioning server and is never enabled as an automatic failure path.
  connected_ = false;
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
    return Status::failure(ErrorCode::kTransport, "Wi-Fi configuration could not be applied");
  }
  const esp_err_t start_error = esp_wifi_start();
  if (start_error != ESP_OK && start_error != ESP_ERR_WIFI_STATE) {
    connected_ = false;
    return Status::failure(ErrorCode::kTransport, "Wi-Fi station could not start");
  }
  if (esp_wifi_connect() != ESP_OK) {
    connected_ = false;
    return Status::failure(ErrorCode::kTransport, "Wi-Fi connection could not start");
  }
  // Event integration must report the eventual link state; beginning a
  // connection is not proof of association, IP, time, or authentication.
  connected_ = false;
  return Status::ok();
}

Status WifiStation::wait_for_connection(std::uint32_t timeout_ms) {
  if (!initialized_) return Status::failure(ErrorCode::kConflict, "Wi-Fi is not initialized");
  const std::int64_t started_ms = esp_timer_get_time() / 1000;
  while (!connected_) {
    const std::int64_t elapsed_ms = esp_timer_get_time() / 1000 - started_ms;
    if (elapsed_ms >= static_cast<std::int64_t>(timeout_ms)) {
      return Status::failure(ErrorCode::kTimeout, "Wi-Fi association timed out");
    }
    vTaskDelay(pdMS_TO_TICKS(100U));
  }
  return Status::ok();
}

Status WifiStation::verify_credentials(const std::string& ssid, const std::string& password,
                                       std::uint32_t timeout_ms) {
  if (!initialized_) return Status::failure(ErrorCode::kConflict, "Wi-Fi is not initialized");
  if (ssid.empty() || ssid.size() > 32U || password.size() > 64U) {
    return Status::failure(ErrorCode::kBounds, "Wi-Fi credentials are invalid");
  }
  wifi_mode_t previous_mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&previous_mode) != ESP_OK) {
    return Status::failure(ErrorCode::kTransport, "Wi-Fi mode could not be read");
  }
  const bool preserve_ap = previous_mode == WIFI_MODE_AP || previous_mode == WIFI_MODE_APSTA;
  wifi_config_t config{};
  std::memcpy(config.sta.ssid, ssid.data(), ssid.size());
  std::memcpy(config.sta.password, password.data(), password.size());
  config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

  // A previous validation can have left the station associated while the
  // Intervals request was being checked. Disconnect that old session before
  // starting a new association attempt so retries are deterministic.
  const bool station_was_connected = connected_;
  if (station_was_connected) {
    const esp_err_t disconnect_error = esp_wifi_disconnect();
    if (disconnect_error != ESP_OK && disconnect_error != ESP_ERR_WIFI_NOT_CONNECT) {
      return Status::failure(ErrorCode::kTransport, "Wi-Fi disconnect failed");
    }
    vTaskDelay(pdMS_TO_TICKS(100U));
  }

  connected_ = false;
  const wifi_mode_t verification_mode = preserve_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  if (esp_wifi_set_mode(verification_mode) != ESP_OK ||
      esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || esp_wifi_connect() != ESP_OK) {
    (void)esp_wifi_set_mode(previous_mode);
    connected_ = false;
    return Status::failure(ErrorCode::kTransport, "Wi-Fi verification could not start");
  }
  const Status result = wait_for_connection(timeout_ms);
  if (!result) {
    (void)esp_wifi_disconnect();
    (void)esp_wifi_set_mode(previous_mode);
    connected_ = false;
  }
  return result;
}

Status WifiStation::disconnect() {
  if (!initialized_) return Status::ok();
  connected_ = false;
  const esp_err_t error = esp_wifi_disconnect();
  return error == ESP_OK || error == ESP_ERR_WIFI_NOT_CONNECT ? Status::ok() : Status::failure(ErrorCode::kTransport, "Wi-Fi disconnect failed");
}

}  // namespace trainer_loop

#else
#include "../host/simulator/services_wifi.inc"

#endif
