#include "trainer_loop/setup.h"

#include "trainer_loop/hash.h"
#include "trainer_loop/limits.h"

#include <array>

namespace trainer_loop {
namespace {

constexpr std::uint64_t kAuthWindowMs = 60000U;
constexpr std::uint32_t kMaxAuthFailuresPerWindow = 8U;

std::uint32_t next_random(std::uint32_t* state) {
  std::uint32_t value = *state;
  if (value == 0U) value = 0x9e3779b9U;
  value ^= value << 13U;
  value ^= value >> 17U;
  value ^= value << 5U;
  *state = value;
  return value;
}

char random_setup_character(std::uint32_t value) {
  static constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  constexpr std::size_t kAlphabetSize = sizeof(kAlphabet) - 1U;
  return kAlphabet[value % kAlphabetSize];
}

bool host_allowed(const std::string& host) {
  return host == "192.168.4.1" || host == "192.168.4.1:80";
}

}  // namespace

SetupCredentials make_setup_credentials(std::uint32_t entropy, std::uint64_t now_ms,
                                        std::uint32_t lifetime_ms) {
  // The entropy value is supplied by the platform RNG. It is not derived from
  // the MAC address. The host implementation accepts deterministic entropy so
  // the security boundary can be unit-tested without pretending it is random.
  std::uint32_t state = entropy ^ static_cast<std::uint32_t>(now_ms) ^
                        static_cast<std::uint32_t>(now_ms >> 32U);
  const std::uint16_t suffix = static_cast<std::uint16_t>(next_random(&state) & 0xffffU);
  SetupCredentials credentials;
  static constexpr char kHex[] = "0123456789ABCDEF";
  credentials.ssid = "TrainerLoop-";
  credentials.ssid.push_back(kHex[(suffix >> 12U) & 0x0fU]);
  credentials.ssid.push_back(kHex[(suffix >> 8U) & 0x0fU]);
  credentials.ssid.push_back(kHex[(suffix >> 4U) & 0x0fU]);
  credentials.ssid.push_back(kHex[suffix & 0x0fU]);
  credentials.passphrase.reserve(16U);
  for (unsigned i = 0U; i < 16U; ++i) credentials.passphrase.push_back(random_setup_character(next_random(&state)));
  credentials.expires_at_ms = now_ms > UINT64_MAX - lifetime_ms ? UINT64_MAX : now_ms + lifetime_ms;
  return credentials;
}

Status SetupSession::begin(const SetupCredentials& credentials, std::string csrf_token) {
  if (credentials.ssid.size() > kMaxWifiSsidBytes || credentials.passphrase.size() < 8U ||
      credentials.passphrase.size() > 63U || credentials.url.empty() || csrf_token.size() < 16U ||
      csrf_token.size() > 128U || credentials.expires_at_ms == 0U) {
    return Status::failure(ErrorCode::kBounds, "setup session credentials are invalid");
  }
  credentials_ = credentials;
  csrf_token_ = std::move(csrf_token);
  open_ = true;
  auth_window_start_ms_ = 0U;
  auth_failures_ = 0U;
  return Status::ok();
}

void SetupSession::close() {
  // Clear the one-time token and setup password so a closed session cannot be
  // accidentally exposed by a later status response.
  credentials_ = SetupCredentials{};
  csrf_token_.clear();
  open_ = false;
  auth_window_start_ms_ = 0U;
  auth_failures_ = 0U;
}

bool SetupSession::active(std::uint64_t now_ms) const {
  return open_ && now_ms < credentials_.expires_at_ms;
}

Status SetupSession::authorize_request(const std::string& host, const std::string& origin,
                                       const std::string& csrf_token, std::uint64_t now_ms,
                                       bool write_request) const {
  if (!active(now_ms)) return Status::failure(ErrorCode::kTimeout, "setup session expired");
  if (auth_window_start_ms_ == 0U || now_ms < auth_window_start_ms_ ||
      now_ms - auth_window_start_ms_ >= kAuthWindowMs) {
    auth_window_start_ms_ = now_ms;
    auth_failures_ = 0U;
  }
  const auto reject = [&](const char* message) {
    if (auth_failures_ < kMaxAuthFailuresPerWindow) ++auth_failures_;
    return Status::failure(ErrorCode::kAuthentication, message);
  };
  if (auth_failures_ >= kMaxAuthFailuresPerWindow) return reject("setup authentication rate limit");
  if (!host_allowed(host)) return reject("unexpected setup Host");
  if (write_request && origin != "http://192.168.4.1") return reject("unexpected setup Origin");
  if (write_request && csrf_token != csrf_token_) return reject("invalid setup CSRF token");
  return Status::ok();
}

Result<SettingsGeneration> SetupTransaction::prepare(const SetupCandidate& candidate) const {
  if (store_ == nullptr) return Result<SettingsGeneration>::failure(ErrorCode::kInvalidArgument, "settings store is null");
  return store_->prepare(candidate.profile, candidate.secrets, store_->current().generation);
}

Status SetupTransaction::commit(const SettingsGeneration& generation, bool wifi_verified,
                                bool time_verified, bool athlete_authenticated) {
  if (store_ == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "settings store is null");
  return store_->commit(generation, wifi_verified, time_verified, athlete_authenticated);
}

std::string SetupTransaction::redacted_status() const {
  if (store_ == nullptr || !store_->has_committed_settings()) return "Setup incomplete";
  return "Setup complete; credentials are not readable";
}

}  // namespace trainer_loop
