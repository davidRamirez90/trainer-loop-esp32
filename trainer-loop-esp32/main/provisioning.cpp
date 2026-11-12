#include "provisioning.h"

#include "trainer_loop/hash.h"

#ifdef ESP_PLATFORM

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

extern const unsigned char _binary_setup_html_start[] asm("_binary_setup_html_start");
extern const unsigned char _binary_setup_html_end[] asm("_binary_setup_html_end");

namespace trainer_loop {
namespace {

constexpr std::size_t kMaxSetupBody = 4096U;
constexpr std::size_t kSetupHttpStackSize = 16U * 1024U;
constexpr std::uint32_t kSetupStopDelayMs = 500U;
ProvisioningServer* g_server = nullptr;

httpd_handle_t server_handle(const ProvisioningServer* server) {
  return reinterpret_cast<httpd_handle_t>(server->http_server());
}

std::string header(httpd_req_t* request, const char* name) {
  const std::size_t length = httpd_req_get_hdr_value_len(request, name);
  if (length == 0U || length > 256U) return {};
  std::string value(length + 1U, '\0');
  if (httpd_req_get_hdr_value_str(request, name, value.data(), value.size()) != ESP_OK) return {};
  value.resize(length);
  return value;
}

esp_err_t send_json(httpd_req_t* request, int status, const std::string& body) {
  httpd_resp_set_status(request, status == 200 ? "200 OK" : "400 Bad Request");
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.c_str(), static_cast<ssize_t>(body.size()));
}

std::string json_string(const cJSON* object, const char* key) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : std::string{};
}

std::optional<double> json_number(const cJSON* object, const char* key) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) && !cJSON_IsNumber(item)) return std::nullopt;
  const char* text = cJSON_IsString(item) ? item->valuestring : nullptr;
  if (text == nullptr) return std::isfinite(item->valuedouble) ? std::optional<double>(item->valuedouble) : std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || *end != '\0' || !std::isfinite(value)) return std::nullopt;
  return value;
}

bool has_json_key(const cJSON* object, const char* key) {
  return cJSON_GetObjectItemCaseSensitive(object, key) != nullptr;
}

std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(std::min<std::size_t>(value.size() + 8U, 256U));
  for (const char character : value) {
    if (escaped.size() >= 240U) break;
    switch (character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (static_cast<unsigned char>(character) < 0x20U) {
          escaped += '?';
        } else {
          escaped += character;
        }
        break;
    }
  }
  return escaped;
}

std::string setup_failure_body(const Status& status) {
  const std::string message = status.error.has_value() && !status.error->message.empty()
                                  ? status.error->message
                                  : "Setup validation failed";
  return "{\"ok\":false,\"message\":\"" + json_escape(message) + "\"}";
}

bool read_json_double(const cJSON* object, const char* key, double* target) {
  if (!has_json_key(object, key)) return true;
  const auto value = json_number(object, key);
  if (!value || target == nullptr) return false;
  *target = *value;
  return true;
}

bool read_json_integer(const cJSON* object, const char* key, int* target) {
  if (!has_json_key(object, key)) return true;
  const auto value = json_number(object, key);
  if (!value || target == nullptr || std::trunc(*value) != *value ||
      *value < static_cast<double>(std::numeric_limits<int>::min()) ||
      *value > static_cast<double>(std::numeric_limits<int>::max())) {
    return false;
  }
  *target = static_cast<int>(*value);
  return true;
}

esp_err_t setup_page(httpd_req_t* request) {
  if (g_server == nullptr || g_server->session() == nullptr) return send_json(request, 400, "{\"ok\":false}");
  const Status authorized = g_server->session()->authorize_request(
      header(request, "Host"), header(request, "Origin"), {},
      static_cast<std::uint64_t>(esp_timer_get_time() / 1000), false);
  if (!authorized) return send_json(request, 400, "{\"ok\":false,\"message\":\"Setup unavailable\"}");
  std::string html(reinterpret_cast<const char*>(_binary_setup_html_start),
                   static_cast<std::size_t>(_binary_setup_html_end - _binary_setup_html_start));
  const std::string marker = "__TRAINER_LOOP_CSRF__";
  const std::size_t position = html.find(marker);
  if (position != std::string::npos) html.replace(position, marker.size(), g_server->session()->csrf_token());
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, html.c_str(), static_cast<ssize_t>(html.size()));
}

esp_err_t setup_status(httpd_req_t* request) {
  if (g_server == nullptr || g_server->session() == nullptr) return send_json(request, 400, "{\"ok\":false}");
  const Status authorized = g_server->session()->authorize_request(
      header(request, "Host"), header(request, "Origin"), {}, esp_timer_get_time() / 1000U, false);
  if (!authorized) return send_json(request, 400, "{\"ok\":false,\"message\":\"Setup unavailable\"}");
  return send_json(request, 200, "{\"ok\":true,\"message\":\"Setup is ready\"}");
}

esp_err_t setup_commit(httpd_req_t* request) {
  if (g_server == nullptr || g_server->session() == nullptr || !g_server->commit_handler()) {
    return send_json(request, 400, "{\"ok\":false,\"message\":\"Setup handler unavailable\"}");
  }
  const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  const Status authorized = g_server->session()->authorize_request(
      header(request, "Host"), header(request, "Origin"), header(request, "X-CSRF-Token"), now_ms, true);
  if (!authorized || request->content_len <= 0 || request->content_len > kMaxSetupBody) {
    return send_json(request, 400, "{\"ok\":false,\"message\":\"Setup request rejected\"}");
  }
  std::string body(static_cast<std::size_t>(request->content_len), '\0');
  std::size_t received = 0U;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received, body.size() - received);
    if (count <= 0) return send_json(request, 400, "{\"ok\":false,\"message\":\"Setup request incomplete\"}");
    received += static_cast<std::size_t>(count);
  }
  cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
  if (root == nullptr) return send_json(request, 400, "{\"ok\":false,\"message\":\"Setup form is invalid\"}");
  SetupCandidate candidate;
  candidate.secrets.wifi_ssid = json_string(root, "wifi_ssid");
  candidate.secrets.wifi_password = json_string(root, "wifi_password");
  candidate.secrets.athlete_id = json_string(root, "athlete_id");
  candidate.secrets.api_key = json_string(root, "api_key");
  candidate.profile.timezone = json_string(root, "timezone");
  if (candidate.profile.timezone.empty()) candidate.profile.timezone = "Europe/Berlin";
  candidate.profile.timezone_rule = json_string(root, "timezone_rule");
  if (candidate.profile.timezone_rule.empty()) {
    candidate.profile.timezone_rule = "CET-1CEST,M3.5.0/2,M10.5.0/3";
  }
  const bool numeric_fields_present = has_json_key(root, "ftp_watts") && has_json_key(root, "rider_kg") &&
                                      has_json_key(root, "bike_kg") && has_json_key(root, "crr") &&
                                      has_json_key(root, "cda");
  const bool numbers_valid = numeric_fields_present && read_json_integer(root, "ftp_watts", &candidate.profile.ftp_watts) &&
                              read_json_double(root, "rider_kg", &candidate.profile.rider_kg) &&
                              read_json_double(root, "bike_kg", &candidate.profile.bike_kg) &&
                              read_json_double(root, "crr", &candidate.profile.crr) &&
                              read_json_double(root, "cda", &candidate.profile.cda);
  cJSON_Delete(root);
  if (!numbers_valid) return send_json(request, 400, "{\"ok\":false,\"message\":\"Setup values are invalid\"}");
  const Status committed = g_server->commit_handler()(candidate);
  if (!committed) return send_json(request, 400, setup_failure_body(committed));
  const esp_err_t response = send_json(request, 200, "{\"ok\":true,\"message\":\"Setup complete\"}");
  if (response == ESP_OK) g_server->request_stop();
  return response;
}

esp_err_t register_handlers(httpd_handle_t server) {
  static httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = setup_page, .user_ctx = nullptr};
  static httpd_uri_t setup = {.uri = "/setup", .method = HTTP_GET, .handler = setup_page, .user_ctx = nullptr};
  static httpd_uri_t status = {.uri = "/api/setup/status", .method = HTTP_GET, .handler = setup_status, .user_ctx = nullptr};
  static httpd_uri_t commit = {.uri = "/api/setup/commit", .method = HTTP_POST, .handler = setup_commit, .user_ctx = nullptr};
  if (httpd_register_uri_handler(server, &root) != ESP_OK) return ESP_FAIL;
  if (httpd_register_uri_handler(server, &setup) != ESP_OK) return ESP_FAIL;
  if (httpd_register_uri_handler(server, &status) != ESP_OK) return ESP_FAIL;
  return httpd_register_uri_handler(server, &commit);
}

}  // namespace

esp_err_t ProvisioningServer::ensure_setup_network() {
  auto* ap = static_cast<esp_netif_t*>(ap_netif_);
  if (ap == nullptr) return ESP_ERR_INVALID_STATE;

  esp_netif_ip_info_t desired{};
  desired.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
  desired.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);
  desired.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);

  esp_netif_ip_info_t current{};
  esp_err_t error = esp_netif_get_ip_info(ap, &current);
  if (error != ESP_OK) return error;
  if (current.ip.addr != desired.ip.addr || current.gw.addr != desired.gw.addr ||
      current.netmask.addr != desired.netmask.addr) {
    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    error = esp_netif_dhcps_get_status(ap, &status);
    if (error != ESP_OK) return error;
    if (status == ESP_NETIF_DHCP_STARTED) {
      error = esp_netif_dhcps_stop(ap);
      if (error != ESP_OK && error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return error;
    }
    error = esp_netif_set_ip_info(ap, &desired);
    if (error != ESP_OK) return error;
  }

  esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
  error = esp_netif_dhcps_get_status(ap, &status);
  if (error != ESP_OK) return error;
  if (status != ESP_NETIF_DHCP_STARTED) {
    error = esp_netif_dhcps_start(ap);
    if (error != ESP_OK && error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) return error;
    error = esp_netif_dhcps_get_status(ap, &status);
    if (error != ESP_OK) return error;
  }
  ESP_LOGI("trainer_setup", "AP IPv4 192.168.4.1/24; DHCP=%s",
           status == ESP_NETIF_DHCP_STARTED ? "started" : "pending");
  return ESP_OK;
}

void ProvisioningServer::event_handler(void* argument, esp_event_base_t event_base,
                                        std::int32_t event_id, void* event_data) {
  auto* server = static_cast<ProvisioningServer*>(argument);
  if (server == nullptr) return;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
    const esp_err_t error = server->ensure_setup_network();
    if (error != ESP_OK) {
      ESP_LOGE("trainer_setup", "AP DHCP setup failed: %s", esp_err_to_name(error));
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    const auto* event = static_cast<const wifi_event_ap_staconnected_t*>(event_data);
    ESP_LOGI("trainer_setup", "setup client associated (AID=%d)", event == nullptr ? 0 : event->aid);
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    const auto* event = static_cast<const wifi_event_ap_stadisconnected_t*>(event_data);
    ESP_LOGI("trainer_setup", "setup client disconnected (reason=%d)",
             event == nullptr ? 0 : event->reason);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
    ESP_LOGI("trainer_setup", "setup DHCP lease assigned");
  }
}

Status ProvisioningServer::start(std::uint64_t now_ms) {
  if (running_) return Status::failure(ErrorCode::kConflict, "setup AP is already running");
  if (session_ == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "setup session is missing");
  credentials_ = make_setup_credentials(esp_random(), now_ms);
  // Use independent platform RNG bytes, not an expanded 32-bit seed.
  std::uint8_t random_bytes[32];
  esp_fill_random(random_bytes, sizeof(random_bytes));
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  for (std::size_t i = 0; i < 16; ++i) credentials_.passphrase[i] = alphabet[random_bytes[i] & 63U];
  std::string csrf;
  static constexpr char hex[] = "0123456789abcdef";
  for (std::size_t i = 16; i < sizeof(random_bytes); ++i) {
    csrf.push_back(hex[random_bytes[i] >> 4]); csrf.push_back(hex[random_bytes[i] & 15U]);
  }
  const Status session_status = session_->begin(credentials_, csrf);
  if (!session_status) return session_status;
  esp_err_t error = esp_netif_init();
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
    session_->close();
    return Status::failure(ErrorCode::kTransport, "Wi-Fi netif init failed");
  }
  error = esp_event_loop_create_default();
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
    session_->close();
    return Status::failure(ErrorCode::kTransport, "event loop init failed");
  }
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  error = esp_wifi_init(&init);
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
    session_->close();
    return Status::failure(ErrorCode::kTransport, "Wi-Fi init failed");
  }
  esp_netif_t* ap = esp_netif_create_default_wifi_ap();
  if (ap == nullptr) {
    session_->close();
    return Status::failure(ErrorCode::kTransport, "setup AP netif could not be created");
  }
  ap_netif_ = ap;
  error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &ProvisioningServer::event_handler, this,
                                               &wifi_event_instance_);
  if (error != ESP_OK) {
    esp_netif_destroy_default_wifi(ap);
    ap_netif_ = nullptr;
    session_->close();
    return Status::failure(ErrorCode::kTransport, "setup Wi-Fi event handler could not start");
  }
  error = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                               &ProvisioningServer::event_handler, this,
                                               &ip_event_instance_);
  if (error != ESP_OK) {
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_instance_);
    wifi_event_instance_ = nullptr;
    esp_netif_destroy_default_wifi(ap);
    ap_netif_ = nullptr;
    session_->close();
    return Status::failure(ErrorCode::kTransport, "setup IP event handler could not start");
  }
  wifi_config_t config{};
  std::strncpy(reinterpret_cast<char*>(config.ap.ssid), credentials_.ssid.c_str(), sizeof(config.ap.ssid) - 1U);
  std::strncpy(reinterpret_cast<char*>(config.ap.password), credentials_.passphrase.c_str(), sizeof(config.ap.password) - 1U);
  config.ap.ssid_len = static_cast<std::uint8_t>(credentials_.ssid.size());
  config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  config.ap.max_connection = 1U;
  if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK ||
      esp_wifi_start() != ESP_OK) {
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                ip_event_instance_);
    ip_event_instance_ = nullptr;
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_instance_);
    wifi_event_instance_ = nullptr;
    (void)esp_wifi_stop();
    esp_netif_destroy_default_wifi(ap_netif_);
    ap_netif_ = nullptr;
    session_->close();
    return Status::failure(ErrorCode::kTransport, "setup AP could not start");
  }
  httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
  http_config.stack_size = kSetupHttpStackSize;
  http_config.max_open_sockets = 1U;
  http_config.recv_wait_timeout = 5U;
  http_config.send_wait_timeout = 5U;
  httpd_handle_t handle = nullptr;
  if (httpd_start(&handle, &http_config) != ESP_OK || register_handlers(handle) != ESP_OK) {
    if (handle != nullptr) (void)httpd_stop(handle);
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                ip_event_instance_);
    ip_event_instance_ = nullptr;
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_instance_);
    wifi_event_instance_ = nullptr;
    (void)esp_wifi_stop();
    esp_netif_destroy_default_wifi(ap_netif_);
    ap_netif_ = nullptr;
    session_->close();
    return Status::failure(ErrorCode::kTransport, "setup HTTP server could not start");
  }
  http_server_ = handle;
  g_server = this;
  stop_requested_ = false;
  running_ = true;
  esp_timer_create_args_t timer{};
  timer.callback = [](void* arg) { static_cast<ProvisioningServer*>(arg)->request_stop(); };
  timer.arg = this;
  timer.name = "setup_expiry";
  const std::uint64_t current_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  const std::uint64_t remaining_ms = credentials_.expires_at_ms > current_ms
      ? credentials_.expires_at_ms - current_ms : 1U;
  if (esp_timer_create(&timer, &expiry_timer_) != ESP_OK ||
      esp_timer_start_once(expiry_timer_, remaining_ms * 1000U) != ESP_OK) {
    (void)stop();
    return Status::failure(ErrorCode::kTransport, "setup expiry timer unavailable");
  }
  ESP_LOGI("trainer_setup", "temporary protected setup AP started for ten minutes");
  return Status::ok();
}

void ProvisioningServer::stop_task_entry(void* argument) {
  auto* server = static_cast<ProvisioningServer*>(argument);
  if (server != nullptr) {
    vTaskDelay(pdMS_TO_TICKS(kSetupStopDelayMs));
    const Status stopped = server->stop();
    if (!stopped) {
      ESP_LOGE("trainer_setup", "setup handoff could not stop the temporary AP: %s",
               stopped.error->message.c_str());
    }
  }
  vTaskDelete(nullptr);
}

void ProvisioningServer::request_stop() {
  if (!running_ || stop_requested_.exchange(true)) return;
  if (xTaskCreate(&ProvisioningServer::stop_task_entry, "setup_handoff", 8192U, this,
                  tskIDLE_PRIORITY + 4U, nullptr) != pdPASS) {
    stop_requested_ = false;
    ESP_LOGE("trainer_setup", "setup handoff task could not start");
  }
}

Status ProvisioningServer::stop() {
  if (!running_) {
    stop_requested_ = false;
    return Status::ok();
  }
  if (expiry_timer_ != nullptr) {
    (void)esp_timer_stop(expiry_timer_);
    (void)esp_timer_delete(expiry_timer_);
    expiry_timer_ = nullptr;
  }
  if (g_server == this) g_server = nullptr;
  if (ip_event_instance_ != nullptr) {
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                ip_event_instance_);
    ip_event_instance_ = nullptr;
  }
  if (wifi_event_instance_ != nullptr) {
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_instance_);
    wifi_event_instance_ = nullptr;
  }
  if (http_server_ != nullptr) {
    (void)httpd_stop(server_handle(this));
    http_server_ = nullptr;
  }
  (void)esp_wifi_stop();
  esp_netif_destroy_default_wifi(ap_netif_);
  session_->close();
  credentials_ = SetupCredentials{};
  ap_netif_ = nullptr;
  running_ = false;
  stop_requested_ = false;
  if (stop_handler_) stop_handler_();
  return Status::ok();
}

ProvisioningServer::~ProvisioningServer() { (void)stop(); }

}  // namespace trainer_loop

#else
#include "../host/simulator/services_provisioning.inc"

#endif
