#include "trainer_loop/settings.h"

#include "trainer_loop/hash.h"
#include "trainer_loop/limits.h"

#include <cmath>
#include <sstream>

namespace trainer_loop {

namespace {

Status validate_saved_device(const std::optional<DeviceIdentity>& device, PeripheralRole role) {
  if (!device.has_value()) return Status::ok();
  if (device->role != role || device->name.size() > 31U || device->address_type > 3U) {
    return Status::failure(ErrorCode::kBounds, "saved BLE identity is invalid");
  }
  bool all_zero = true;
  for (const std::uint8_t byte : device->address) {
    if (byte != 0U) {
      all_zero = false;
      break;
    }
  }
  if (all_zero) return Status::failure(ErrorCode::kBounds, "saved BLE identity has no address");
  return Status::ok();
}

}  // namespace

Status validate_profile(const ProfileSettings& profile) {
  if (profile.ftp_watts < kFtpMinWatts || profile.ftp_watts > kFtpMaxWatts) {
    return Status::failure(ErrorCode::kBounds, "FTP must be between 50 and 600 W");
  }
  if (!std::isfinite(profile.rider_kg) || profile.rider_kg < kRiderMinKg || profile.rider_kg > kRiderMaxKg) {
    return Status::failure(ErrorCode::kBounds, "rider weight is outside 30–200 kg");
  }
  if (!std::isfinite(profile.bike_kg) || profile.bike_kg < kBikeMinKg || profile.bike_kg > kBikeMaxKg) {
    return Status::failure(ErrorCode::kBounds, "bike weight is outside 5–15 kg");
  }
  if (!std::isfinite(profile.crr) || profile.crr < kCrrMin || profile.crr > kCrrMax) {
    return Status::failure(ErrorCode::kBounds, "Crr is outside 0.002–0.010");
  }
  if (!std::isfinite(profile.cda) || profile.cda < kCdAMin || profile.cda > kCdAMax) {
    return Status::failure(ErrorCode::kBounds, "CdA is outside 0.15–0.60 m²");
  }
  if (profile.timezone.empty() || profile.timezone.size() > kMaxTimezoneBytes ||
      profile.timezone_rule.empty() || profile.timezone_rule.size() > 96U) {
    return Status::failure(ErrorCode::kBounds, "timezone or POSIX rule is invalid");
  }
  return Status::ok();
}

Status validate_secrets(const Secrets& secrets) {
  if (secrets.wifi_ssid.empty() || secrets.wifi_ssid.size() > kMaxWifiSsidBytes) {
    return Status::failure(ErrorCode::kBounds, "Wi-Fi SSID is invalid");
  }
  if (secrets.wifi_password.size() > kMaxWifiPasswordBytes) {
    return Status::failure(ErrorCode::kBounds, "Wi-Fi password is too long");
  }
  if (secrets.athlete_id.empty() || secrets.athlete_id.size() > kMaxAthleteIdBytes) {
    return Status::failure(ErrorCode::kBounds, "athlete ID is invalid");
  }
  if (secrets.api_key.empty() || secrets.api_key.size() > kMaxApiKeyBytes) {
    return Status::failure(ErrorCode::kBounds, "API key is invalid");
  }
  if (secrets.local_admin_secret.size() > 128U) {
    return Status::failure(ErrorCode::kBounds, "local admin secret is too long");
  }
  return Status::ok();
}

std::uint32_t settings_crc(const SettingsGeneration& settings) {
  std::ostringstream canonical;
  const auto append_device = [&canonical](const std::optional<DeviceIdentity>& device) {
    canonical << '|' << (device.has_value() ? 1 : 0);
    if (!device.has_value()) return;
    canonical << '|' << static_cast<unsigned>(device->address_type);
    for (const std::uint8_t byte : device->address) canonical << '|' << static_cast<unsigned>(byte);
    canonical << '|' << device->name;
  };
  canonical << settings.schema_version << '|' << settings.generation << '|'
            << settings.profile.ftp_watts << '|' << settings.profile.rider_kg << '|'
            << settings.profile.bike_kg << '|' << settings.profile.crr << '|'
            << settings.profile.cda << '|' << settings.profile.ftp_local_override << '|'
            << settings.profile.weight_local_override << '|' << settings.profile.timezone << '|'
            << settings.profile.timezone_rule << '|' << settings.profile.timezone_rule_version << '|'
            << settings.provisioning_complete;
  append_device(settings.trainer_device);
  append_device(settings.heart_rate_device);
  // Secrets are intentionally excluded from the diagnostic checksum string;
  // NVS provides record integrity. At-rest encryption is a separate release
  // policy; development builds must not claim encrypted/authenticated storage.
  return fnv1a32(canonical.str());
}

SettingsStore::SettingsStore() {
  current_.schema_version = kSettingsSchemaVersion;
  current_.generation = 0U;
  current_.provisioning_complete = false;
  current_.crc = settings_crc(current_);
}

Result<SettingsGeneration> SettingsStore::prepare(const ProfileSettings& profile,
                                                   const Secrets& secrets,
                                                   std::uint32_t expected_generation) const {
  if (expected_generation != current_.generation) {
    return Result<SettingsGeneration>::failure(ErrorCode::kConflict, "settings generation changed");
  }
  if (current_.generation == UINT32_MAX) {
    return Result<SettingsGeneration>::failure(ErrorCode::kOverflow, "settings generation is exhausted");
  }
  const Status profile_status = validate_profile(profile);
  if (!profile_status) return Result<SettingsGeneration>::failure(profile_status.error->code, profile_status.error->message);
  const Status secret_status = validate_secrets(secrets);
  if (!secret_status) return Result<SettingsGeneration>::failure(secret_status.error->code, secret_status.error->message);
  SettingsGeneration candidate;
  candidate.schema_version = kSettingsSchemaVersion;
  candidate.generation = current_.generation + 1U;
  candidate.profile = profile;
  candidate.secrets = secrets;
  candidate.trainer_device = current_.trainer_device;
  candidate.heart_rate_device = current_.heart_rate_device;
  candidate.provisioning_complete = true;
  candidate.crc = settings_crc(candidate);
  return Result<SettingsGeneration>::success(std::move(candidate));
}

Status SettingsStore::commit(const SettingsGeneration& candidate, bool wifi_verified,
                             bool time_verified, bool athlete_authenticated) {
  if (!wifi_verified || !time_verified || !athlete_authenticated) {
    return Status::failure(ErrorCode::kAuthentication, "setup verification is incomplete");
  }
  if (current_.generation == UINT32_MAX || candidate.generation != current_.generation + 1U ||
      candidate.schema_version != kSettingsSchemaVersion || !candidate.provisioning_complete) {
    return Status::failure(ErrorCode::kConflict, "settings generation is stale");
  }
  const Status profile_status = validate_profile(candidate.profile);
  if (!profile_status) return profile_status;
  const Status secret_status = validate_secrets(candidate.secrets);
  if (!secret_status) return secret_status;
  const Status trainer_status = validate_saved_device(candidate.trainer_device, PeripheralRole::kTrainer);
  if (!trainer_status) return trainer_status;
  const Status heart_rate_status = validate_saved_device(candidate.heart_rate_device, PeripheralRole::kHeartRate);
  if (!heart_rate_status) return heart_rate_status;
  if (candidate.crc != settings_crc(candidate)) {
    return Status::failure(ErrorCode::kStorage, "settings checksum mismatch");
  }
  current_ = candidate;
  return Status::ok();
}

Status SettingsStore::restore(const SettingsGeneration& loaded) {
  if (loaded.schema_version != kSettingsSchemaVersion) {
    return Status::failure(ErrorCode::kUnsupported, "settings schema is unsupported");
  }
  if (loaded.provisioning_complete) {
    const Status profile_status = validate_profile(loaded.profile);
    if (!profile_status) return profile_status;
    const Status secret_status = validate_secrets(loaded.secrets);
    if (!secret_status) return secret_status;
  }
  const Status trainer_status = validate_saved_device(loaded.trainer_device, PeripheralRole::kTrainer);
  if (!trainer_status) return trainer_status;
  const Status heart_rate_status = validate_saved_device(loaded.heart_rate_device, PeripheralRole::kHeartRate);
  if (!heart_rate_status) return heart_rate_status;
  if (loaded.crc != settings_crc(loaded)) return Status::failure(ErrorCode::kStorage, "persisted settings checksum mismatch");
  if (loaded.generation < current_.generation) return Status::failure(ErrorCode::kConflict, "persisted settings generation regressed");
  current_ = loaded;
  return Status::ok();
}

Status SettingsStore::update_profile(const ProfileSettings& profile, std::uint32_t expected_generation) {
  if (!current_.provisioning_complete) return Status::failure(ErrorCode::kConflict, "settings are not provisioned");
  if (expected_generation != current_.generation) return Status::failure(ErrorCode::kConflict, "settings generation changed");
  if (current_.generation == UINT32_MAX) return Status::failure(ErrorCode::kOverflow, "settings generation is exhausted");
  const Status valid = validate_profile(profile);
  if (!valid) return valid;
  SettingsGeneration next = current_;
  next.generation += 1U;
  next.profile = profile;
  next.crc = settings_crc(next);
  current_ = std::move(next);
  return Status::ok();
}

Status SettingsStore::update_device(PeripheralRole role, std::optional<DeviceIdentity> device,
                                    std::uint32_t expected_generation) {
  if (!current_.provisioning_complete) return Status::failure(ErrorCode::kConflict, "settings are not provisioned");
  if (expected_generation != current_.generation) return Status::failure(ErrorCode::kConflict, "settings generation changed");
  if (current_.generation == UINT32_MAX) return Status::failure(ErrorCode::kOverflow, "settings generation is exhausted");
  const Status valid = role == PeripheralRole::kTrainer
                           ? validate_saved_device(device, PeripheralRole::kTrainer)
                           : validate_saved_device(device, PeripheralRole::kHeartRate);
  if (!valid) return valid;
  SettingsGeneration next = current_;
  ++next.generation;
  if (role == PeripheralRole::kTrainer) next.trainer_device = std::move(device);
  else next.heart_rate_device = std::move(device);
  next.crc = settings_crc(next);
  current_ = std::move(next);
  return Status::ok();
}

void SettingsStore::clear() {
  current_ = SettingsGeneration{};
  current_.schema_version = kSettingsSchemaVersion;
  current_.crc = settings_crc(current_);
}

}  // namespace trainer_loop
