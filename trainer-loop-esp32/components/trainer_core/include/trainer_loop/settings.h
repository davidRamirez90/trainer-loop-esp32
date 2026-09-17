#pragma once

#include "trainer_loop/ble_manager.h"
#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <cstdint>
#include <optional>
#include <string>

namespace trainer_loop {

constexpr std::uint32_t kSettingsSchemaVersion = 1U;

Status validate_profile(const ProfileSettings& profile);
Status validate_secrets(const Secrets& secrets);

struct SettingsGeneration {
  std::uint32_t schema_version = kSettingsSchemaVersion;
  std::uint32_t generation = 0U;
  ProfileSettings profile;
  Secrets secrets;
  // Explicitly selected peers are persisted by address and address type.
  // Device names are display metadata only and never drive reconnection.
  std::optional<DeviceIdentity> trainer_device;
  std::optional<DeviceIdentity> heart_rate_device;
  bool provisioning_complete = false;
  std::uint32_t crc = 0U;
};

std::uint32_t settings_crc(const SettingsGeneration& settings);

class SettingsStore {
 public:
  SettingsStore();

  const SettingsGeneration& current() const { return current_; }
  bool has_committed_settings() const { return current_.provisioning_complete; }

  Result<SettingsGeneration> prepare(const ProfileSettings& profile, const Secrets& secrets,
                                     std::uint32_t expected_generation) const;
  Status commit(const SettingsGeneration& candidate, bool wifi_verified, bool time_verified,
                bool athlete_authenticated);
  Status restore(const SettingsGeneration& loaded);
  Status update_profile(const ProfileSettings& profile, std::uint32_t expected_generation);
  Status update_device(PeripheralRole role, std::optional<DeviceIdentity> device,
                       std::uint32_t expected_generation);
  void clear();

 private:
  SettingsGeneration current_;
};

}  // namespace trainer_loop
