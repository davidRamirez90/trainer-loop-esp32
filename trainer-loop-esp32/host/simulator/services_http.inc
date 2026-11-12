#include "intervals_http.h"
#include "platform_services.h"

#include <algorithm>
#include <limits>

namespace trainer_loop {
namespace {
constexpr std::size_t kMaxHostResponseBytes = 256U * 1024U;

std::string query_value(const std::string& query, const std::string& key) {
  const std::string marker = key + "=";
  const auto at = query.find(marker);
  if (at == std::string::npos) return {};
  const auto begin = at + marker.size();
  const auto end = query.find('&', begin);
  return query.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

HostPlatformServices::HttpFixture synthetic_response(const HttpRequest& request) {
  HostPlatformServices::HttpFixture response;
  if (request.method == "POST") {
    response.status = 201;
    response.body = "{\"id\":\"host-activity-1\"}";
  } else if (request.path.find("/download.zwo") != std::string::npos) {
    response.body = "<?xml version=\"1.0\"?><workout_file><name>Local Endurance</name><workout><SteadyState Duration=\"1800\" Power=\"0.65\"/></workout></workout_file>";
  } else if (request.path.size() >= 7U && request.path.compare(request.path.size() - 7U, 7U, "/events") == 0) {
    const std::string date = query_value(request.query, "oldest");
    response.body = "[{\"id\":\"host-workout\",\"name\":\"Local Endurance\",\"start_date_local\":\"" +
                    (date.empty() ? std::string("2026-09-30") : date) +
                    "T07:00:00\",\"sport\":\"Ride\"}]";
  } else if (request.path.find("/api/v1/athlete/") == 0U) {
    response.body = "{\"ftp\":250,\"icu_weight\":75}";
  } else {
    response.status = 404;
    response.body = "not found";
  }
  return response;
}
}

Result<HttpResponse> IntervalsHttpClient::execute(const HttpRequest& request) const {
  if ((request.method != "GET" && request.method != "POST") || request.path.empty() ||
      request.path.front() != '/' || request.body.size() > 2U * 1024U * 1024U)
    return Result<HttpResponse>::failure(ErrorCode::kInvalidArgument, "Intervals request is invalid");
  auto& services = HostPlatformServices::instance();
  services.record_event("http.request method=" + request.method + " path=" + request.path);
  if (!services.wifi_available() || !services.http_available())
    return Result<HttpResponse>::failure(ErrorCode::kTransport, "local Intervals fixture transport is unavailable");
  const auto* fixture = services.http_fixture(request.method, request.path);
  const auto response = fixture == nullptr ? synthetic_response(request) : *fixture;
  if (response.body.size() > kMaxHostResponseBytes)
    return Result<HttpResponse>::failure(ErrorCode::kBounds, "Intervals response exceeded the device limit");
  services.record_event("http.response status=" + std::to_string(response.status) +
                        " bytes=" + std::to_string(response.body.size()));
  return Result<HttpResponse>::success(HttpResponse{response.status, response.body, response.retry_after_s});
}

}  // namespace trainer_loop
