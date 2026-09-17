#pragma once

#include "trainer_loop/result.h"

#include <cstdint>
#include <string>

namespace trainer_loop {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string authorization;
  std::string content_type;
  std::string body;
};

std::string url_encode_component(const std::string& value);
std::string basic_auth_header(const std::string& api_key);
std::string sanitized_fit_filename(const std::string& session_uuid);

HttpRequest build_athlete_request(const std::string& athlete_id, const std::string& api_key);
HttpRequest build_today_events_request(const std::string& athlete_id, const std::string& api_key,
                                       const std::string& local_date);
HttpRequest build_zwo_request(const std::string& athlete_id, const std::string& api_key,
                              const std::string& event_id);
HttpRequest build_upload_request(const std::string& athlete_id, const std::string& api_key,
                                 const std::string& session_uuid, const std::string& fit_bytes);

enum class UploadHttpClass : std::uint8_t {
  kAccepted,
  kDuplicate,
  kAuthentication,
  kRateLimited,
  kTransient,
  kPermanent,
};

struct UploadHttpResult {
  UploadHttpClass classification = UploadHttpClass::kPermanent;
  int status = 0;
  std::uint32_t retry_after_s = 0U;
  std::string remote_activity_id;
};

UploadHttpResult classify_upload_response(int status, const std::string& body,
                                          std::uint32_t retry_after_s = 0U);

}  // namespace trainer_loop
