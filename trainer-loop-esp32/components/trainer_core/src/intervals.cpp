#include "trainer_loop/intervals.h"

#include "trainer_loop/limits.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <utility>

namespace trainer_loop {
namespace {

bool is_unreserved(unsigned char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' || value == '~';
}

std::string base64(const std::string& input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((input.size() + 2U) / 3U) * 4U);
  std::size_t position = 0U;
  while (position < input.size()) {
    const std::size_t count = std::min<std::size_t>(3U, input.size() - position);
    const std::uint32_t a = static_cast<unsigned char>(input[position++]);
    const std::uint32_t b = count > 1U ? static_cast<unsigned char>(input[position++]) : 0U;
    const std::uint32_t c = count > 2U ? static_cast<unsigned char>(input[position++]) : 0U;
    output.push_back(kAlphabet[(a >> 2U) & 0x3fU]);
    output.push_back(kAlphabet[((a & 0x03U) << 4U) | ((b >> 4U) & 0x0fU)]);
    output.push_back(count > 1U ? kAlphabet[((b & 0x0fU) << 2U) | ((c >> 6U) & 0x03U)] : '=');
    output.push_back(count > 2U ? kAlphabet[c & 0x3fU] : '=');
  }
  return output;
}

std::string lower_ascii(std::string value) {
  for (char& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return value;
}

HttpRequest authenticated(std::string method, std::string path, std::string api_key) {
  HttpRequest request;
  request.method = std::move(method);
  request.path = std::move(path);
  request.authorization = basic_auth_header(api_key);
  return request;
}

std::string response_activity_id(const std::string& body) {
  const std::string key = "\"id\"";
  std::size_t position = body.find(key);
  while (position != std::string::npos) {
    std::size_t cursor = position + key.size();
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor]))) ++cursor;
    if (cursor >= body.size() || body[cursor] != ':') {
      position = body.find(key, position + 1U);
      continue;
    }
    ++cursor;
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor]))) ++cursor;
    if (cursor >= body.size()) return {};
    if (body[cursor] == '"') {
      ++cursor;
      std::string value;
      while (cursor < body.size() && body[cursor] != '"') {
        if (body[cursor] == '\\') {
          if (++cursor >= body.size()) return {};
          const char escaped = body[cursor];
          if (escaped == '"' || escaped == '\\' || escaped == '/') value.push_back(escaped);
          else if (escaped == 'b') value.push_back('\b');
          else if (escaped == 'f') value.push_back('\f');
          else if (escaped == 'n') value.push_back('\n');
          else if (escaped == 'r') value.push_back('\r');
          else if (escaped == 't') value.push_back('\t');
          else return {};
        } else {
          value.push_back(body[cursor]);
        }
        if (value.size() > 96U) return {};
        ++cursor;
      }
      return cursor < body.size() && !value.empty() ? value : std::string{};
    }
    const std::size_t start = cursor;
    while (cursor < body.size() && body[cursor] >= '0' && body[cursor] <= '9') ++cursor;
    if (cursor > start && cursor - start <= 96U) return body.substr(start, cursor - start);
    return {};
  }
  return {};
}

}  // namespace

std::string url_encode_component(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string output;
  output.reserve(value.size());
  for (const char raw_character : value) {
    const unsigned char character = static_cast<unsigned char>(raw_character);
    if (is_unreserved(character)) output.push_back(static_cast<char>(character));
    else {
      output.push_back('%');
      output.push_back(kHex[(character >> 4U) & 0x0fU]);
      output.push_back(kHex[character & 0x0fU]);
    }
  }
  return output;
}

std::string basic_auth_header(const std::string& api_key) {
  // Intervals.icu personal API keys use the literal username "API_KEY"
  // and the generated API key as the Basic Auth password.
  return "Basic " + base64("API_KEY:" + api_key);
}

std::string sanitized_fit_filename(const std::string& session_uuid) {
  std::string safe;
  for (const char raw_character : session_uuid) {
    const unsigned char character = static_cast<unsigned char>(raw_character);
    if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9') || character == '-' || character == '_') {
      if (safe.size() < 48U) safe.push_back(static_cast<char>(character));
    }
  }
  if (safe.empty()) safe = "session";
  return safe + ".fit";
}

HttpRequest build_athlete_request(const std::string& athlete_id, const std::string& api_key) {
  return authenticated("GET", "/api/v1/athlete/" + url_encode_component(athlete_id), api_key);
}

HttpRequest build_today_events_request(const std::string& athlete_id, const std::string& api_key,
                                       const std::string& local_date) {
  HttpRequest request = build_athlete_request(athlete_id, api_key);
  request.path += "/events";
  request.query = "oldest=" + url_encode_component(local_date) + "&newest=" +
                  url_encode_component(local_date) + "&category=WORKOUT";
  return request;
}

HttpRequest build_zwo_request(const std::string& athlete_id, const std::string& api_key,
                              const std::string& event_id) {
  HttpRequest request = build_athlete_request(athlete_id, api_key);
  request.path += "/events/" + url_encode_component(event_id) + "/download.zwo";
  return request;
}

HttpRequest build_upload_request(const std::string& athlete_id, const std::string& api_key,
                                 const std::string& session_uuid, const std::string& fit_bytes) {
  HttpRequest request = build_athlete_request(athlete_id, api_key);
  request.method = "POST";
  request.path += "/activities";
  request.content_type = "multipart/form-data; boundary=TrainerLoopBoundary";
  request.body = "--TrainerLoopBoundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" +
                 sanitized_fit_filename(session_uuid) +
                 "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + fit_bytes +
                 "\r\n--TrainerLoopBoundary--\r\n";
  return request;
}

UploadHttpResult classify_upload_response(int status, const std::string& body,
                                          std::uint32_t retry_after_s) {
  UploadHttpResult result;
  result.status = status;
  result.retry_after_s = retry_after_s;
  result.remote_activity_id = response_activity_id(body);
  if (status >= 200 && status < 300) {
    result.classification = UploadHttpClass::kAccepted;
  } else if (status == 422 && lower_ascii(body) == "duplicate activity") {
    // Exact development fixture only; never classify arbitrary error text
    // mentioning duplicate fields as success. Live service wording is a gate.
    result.classification = UploadHttpClass::kDuplicate;
  } else if (status == 401 || status == 403) {
    result.classification = UploadHttpClass::kAuthentication;
  } else if (status == 429) {
    result.classification = UploadHttpClass::kRateLimited;
  } else if (status == 408 || status >= 500 || status == 0) {
    result.classification = UploadHttpClass::kTransient;
  } else {
    result.classification = UploadHttpClass::kPermanent;
  }
  return result;
}

}  // namespace trainer_loop
