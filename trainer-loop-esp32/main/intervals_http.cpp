#include "intervals_http.h"

#ifdef ESP_PLATFORM

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <limits>

namespace trainer_loop {
namespace {

constexpr std::size_t kMaxResponseBytes = 256U * 1024U;
constexpr char kOrigin[] = "https://intervals.icu";

struct ResponseBuffer {
  std::string body;
  bool too_large = false;
  std::uint32_t retry_after_s = 0U;
};

bool equals_header(const char* left, const char* right) {
  if (left == nullptr || right == nullptr) return false;
  while (*left != '\0' && *right != '\0') {
    if (std::tolower(static_cast<unsigned char>(*left)) != std::tolower(static_cast<unsigned char>(*right))) return false;
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

esp_err_t event_handler(esp_http_client_event_t* event) {
  if (event == nullptr || event->user_data == nullptr) return ESP_OK;
  auto* response = static_cast<ResponseBuffer*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_HEADER && equals_header(event->header_key, "Retry-After") &&
      event->header_value != nullptr) {
    std::uint64_t seconds = 0U;
    const char* begin = event->header_value;
    const char* end = begin + std::strlen(begin);
    const auto parsed = std::from_chars(begin, end, seconds, 10);
    if (parsed.ec == std::errc{} && parsed.ptr == end && seconds <= std::numeric_limits<std::uint32_t>::max()) {
      response->retry_after_s = static_cast<std::uint32_t>(seconds);
    }
  } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr && event->data_len > 0) {
    const std::size_t length = static_cast<std::size_t>(event->data_len);
    if (response->body.size() + length > kMaxResponseBytes) {
      response->too_large = true;
      return ESP_FAIL;
    }
    response->body.append(static_cast<const char*>(event->data), length);
  }
  return ESP_OK;
}

}  // namespace

Result<HttpResponse> IntervalsHttpClient::execute(const HttpRequest& request) const {
  if ((request.method != "GET" && request.method != "POST") || request.path.empty() || request.path.front() != '/' ||
      request.body.size() > 2U * 1024U * 1024U) {
    return Result<HttpResponse>::failure(ErrorCode::kInvalidArgument, "Intervals request is invalid");
  }
  ResponseBuffer response;
  const std::string url = std::string(kOrigin) + request.path + (request.query.empty() ? "" : "?" + request.query);
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.event_handler = event_handler;
  config.user_data = &response;
  config.timeout_ms = 15000;
  config.disable_auto_redirect = true;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return Result<HttpResponse>::failure(ErrorCode::kTransport, "Intervals HTTP init failed");
  esp_err_t error = esp_http_client_set_method(client, request.method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET);
  if (error == ESP_OK && !request.authorization.empty()) {
    error = esp_http_client_set_header(client, "Authorization", request.authorization.c_str());
  }
  if (error == ESP_OK && !request.content_type.empty()) {
    error = esp_http_client_set_header(client, "Content-Type", request.content_type.c_str());
  }
  if (error == ESP_OK && !request.body.empty()) {
    error = esp_http_client_set_post_field(client, request.body.data(), static_cast<int>(request.body.size()));
  }
  if (error == ESP_OK) error = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (response.too_large) {
    return Result<HttpResponse>::failure(ErrorCode::kTransport, "Intervals HTTP request failed");
  }

  // Some API error responses (notably 401 without a WWW-Authenticate
  // challenge) make esp_http_client_perform return an error even though a
  // useful HTTP status and body were received. Preserve HTTP error responses
  // so setup can distinguish rejected credentials from transport failures.
  if (error != ESP_OK && (status < 400 || status >= 600)) {
    return Result<HttpResponse>::failure(ErrorCode::kTransport, "Intervals HTTP request failed");
  }

  return Result<HttpResponse>::success(HttpResponse{status, std::move(response.body), response.retry_after_s});
}

}  // namespace trainer_loop

#else

namespace trainer_loop {
Result<HttpResponse> IntervalsHttpClient::execute(const HttpRequest&) const {
  return Result<HttpResponse>::failure(ErrorCode::kUnsupported, "Intervals HTTP requires ESP-IDF");
}
}  // namespace trainer_loop

#endif
