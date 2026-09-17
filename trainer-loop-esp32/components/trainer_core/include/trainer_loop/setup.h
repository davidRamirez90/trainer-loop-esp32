#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"
#include "trainer_loop/settings.h"

#include <cstdint>
#include <string>

namespace trainer_loop {

struct SetupCredentials {
  std::string ssid;
  std::string passphrase;
  std::string url = "http://192.168.4.1";
  std::uint64_t expires_at_ms = 0U;
};

SetupCredentials make_setup_credentials(std::uint32_t entropy, std::uint64_t now_ms,
                                        std::uint32_t lifetime_ms = 600000U);

class SetupSession {
 public:
  Status begin(const SetupCredentials& credentials, std::string csrf_token);
  void close();
  bool active(std::uint64_t now_ms) const;

  Status authorize_request(const std::string& host, const std::string& origin,
                           const std::string& csrf_token, std::uint64_t now_ms,
                           bool write_request) const;
  const SetupCredentials& credentials() const { return credentials_; }
  const std::string& csrf_token() const { return csrf_token_; }

 private:
  SetupCredentials credentials_;
  std::string csrf_token_;
  bool open_ = false;
  mutable std::uint64_t auth_window_start_ms_ = 0U;
  mutable std::uint32_t auth_failures_ = 0U;
};

struct SetupCandidate {
  ProfileSettings profile;
  Secrets secrets;
};

class SetupTransaction {
 public:
  explicit SetupTransaction(SettingsStore* store) : store_(store) {}

  Result<SettingsGeneration> prepare(const SetupCandidate& candidate) const;
  Status commit(const SettingsGeneration& generation, bool wifi_verified,
                bool time_verified, bool athlete_authenticated);
  std::string redacted_status() const;

 private:
  SettingsStore* store_ = nullptr;
};

}  // namespace trainer_loop
