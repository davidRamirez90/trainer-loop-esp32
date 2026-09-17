#include "settings_nvs.h"

#ifdef ESP_PLATFORM

#include "nvs.h"
#include "nvs_flash.h"

#include <array>
#include <cstdint>
#include <string>

namespace trainer_loop {
namespace {

// NVS commits are atomic per namespace, not across namespaces. Each logical
// generation therefore lives in a paired slot; the previous slot remains a
// complete recovery point while the next slot is being written.
constexpr std::array<const char*, 2U> kValueNamespaces = {"trloop_val_a", "trloop_val_b"};
constexpr std::array<const char*, 2U> kSecretNamespaces = {"trloop_sec_a", "trloop_sec_b"};

esp_err_t put_string(nvs_handle_t handle, const char* key, const std::string& value) {
  return nvs_set_str(handle, key, value.c_str());
}

template <typename T>
esp_err_t put_blob(nvs_handle_t handle, const char* key, const T& value) {
  return nvs_set_blob(handle, key, &value, sizeof(value));
}

template <typename T>
esp_err_t get_blob(nvs_handle_t handle, const char* key, T* value) {
  std::size_t size = sizeof(T);
  const esp_err_t error = nvs_get_blob(handle, key, value, &size);
  return error == ESP_OK && size == sizeof(T) ? ESP_OK : (error == ESP_OK ? ESP_ERR_NVS_INVALID_LENGTH : error);
}

Status read_string(nvs_handle_t handle, const char* key, std::string* value, bool required,
                   std::size_t maximum) {
  std::size_t length = 0U;
  const esp_err_t first = nvs_get_str(handle, key, nullptr, &length);
  if (first == ESP_ERR_NVS_NOT_FOUND && !required) return Status::ok();
  if (first != ESP_OK || length == 0U || length > maximum + 1U) {
    return Status::failure(ErrorCode::kStorage, "NVS string is invalid");
  }
  std::string buffer(length, '\0');
  if (nvs_get_str(handle, key, buffer.data(), &length) != ESP_OK) return Status::failure(ErrorCode::kStorage, "NVS string read failed");
  buffer.resize(length > 0U ? length - 1U : 0U);
  *value = std::move(buffer);
  return Status::ok();
}

esp_err_t erase_optional(nvs_handle_t handle, const char* key) {
  const esp_err_t error = nvs_erase_key(handle, key);
  return error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
}

esp_err_t write_device(nvs_handle_t values, const char* prefix,
                       const std::optional<DeviceIdentity>& device) {
  const std::string address_key = std::string(prefix) + "_addr";
  const std::string type_key = std::string(prefix) + "_type";
  const std::string name_key = std::string(prefix) + "_name";
  if (!device.has_value()) {
    esp_err_t error = erase_optional(values, address_key.c_str());
    if (error == ESP_OK) error = erase_optional(values, type_key.c_str());
    if (error == ESP_OK) error = erase_optional(values, name_key.c_str());
    return error;
  }
  esp_err_t error = nvs_set_blob(values, address_key.c_str(), device->address.data(), device->address.size());
  if (error == ESP_OK) error = nvs_set_u8(values, type_key.c_str(), device->address_type);
  if (error == ESP_OK) error = nvs_set_str(values, name_key.c_str(), device->name.c_str());
  return error;
}

bool read_device(nvs_handle_t values, const char* prefix, PeripheralRole role,
                 std::optional<DeviceIdentity>* device) {
  if (device == nullptr) return false;
  const std::string address_key = std::string(prefix) + "_addr";
  const std::string type_key = std::string(prefix) + "_type";
  const std::string name_key = std::string(prefix) + "_name";
  DeviceIdentity identity;
  std::size_t address_size = identity.address.size();
  const esp_err_t address_error = nvs_get_blob(values, address_key.c_str(), identity.address.data(), &address_size);
  if (address_error == ESP_ERR_NVS_NOT_FOUND) {
    device->reset();
    return true;
  }
  if (address_error != ESP_OK || address_size != identity.address.size() ||
      nvs_get_u8(values, type_key.c_str(), &identity.address_type) != ESP_OK) {
    return false;
  }
  if (!read_string(values, name_key.c_str(), &identity.name, false, 31U)) return false;
  identity.role = role;
  *device = std::move(identity);
  return true;
}

bool open_slot(std::size_t slot, nvs_open_mode_t mode, nvs_handle_t* values, nvs_handle_t* secrets) {
  if (slot >= kValueNamespaces.size() || values == nullptr || secrets == nullptr) return false;
  *values = 0;
  *secrets = 0;
  if (nvs_open(kValueNamespaces[slot], mode, values) != ESP_OK ||
      nvs_open(kSecretNamespaces[slot], mode, secrets) != ESP_OK) {
    if (*values != 0) nvs_close(*values);
    if (*secrets != 0) nvs_close(*secrets);
    *values = 0;
    *secrets = 0;
    return false;
  }
  return true;
}

void close_slot(nvs_handle_t values, nvs_handle_t secrets) {
  if (secrets != 0) nvs_close(secrets);
  if (values != 0) nvs_close(values);
}

esp_err_t write_values(nvs_handle_t values, const SettingsGeneration& settings) {
  esp_err_t error = nvs_set_u32(values, "schema", settings.schema_version);
  error = error == ESP_OK ? nvs_set_u32(values, "generation", settings.generation) : error;
  error = error == ESP_OK ? nvs_set_u8(values, "complete", settings.provisioning_complete ? 1U : 0U) : error;
  error = error == ESP_OK ? nvs_set_i32(values, "ftp", settings.profile.ftp_watts) : error;
  error = error == ESP_OK ? put_blob(values, "rider_kg", settings.profile.rider_kg) : error;
  error = error == ESP_OK ? put_blob(values, "bike_kg", settings.profile.bike_kg) : error;
  error = error == ESP_OK ? put_blob(values, "crr", settings.profile.crr) : error;
  error = error == ESP_OK ? put_blob(values, "cda", settings.profile.cda) : error;
  error = error == ESP_OK ? nvs_set_u8(values, "ftp_override", settings.profile.ftp_local_override ? 1U : 0U) : error;
  error = error == ESP_OK ? nvs_set_u8(values, "weight_override", settings.profile.weight_local_override ? 1U : 0U) : error;
  error = error == ESP_OK ? nvs_set_u32(values, "tz_rule_ver", settings.profile.timezone_rule_version) : error;
  error = error == ESP_OK ? nvs_set_u32(values, "crc", settings.crc) : error;
  error = error == ESP_OK ? put_string(values, "timezone", settings.profile.timezone) : error;
  error = error == ESP_OK ? put_string(values, "tz_rule", settings.profile.timezone_rule) : error;
  error = error == ESP_OK ? write_device(values, "trainer", settings.trainer_device) : error;
  return error == ESP_OK ? write_device(values, "heart", settings.heart_rate_device) : error;
}

esp_err_t write_secrets(nvs_handle_t secrets, const SettingsGeneration& settings) {
  esp_err_t error = put_string(secrets, "wifi_ssid", settings.secrets.wifi_ssid);
  error = error == ESP_OK ? put_string(secrets, "wifi_password", settings.secrets.wifi_password) : error;
  error = error == ESP_OK ? put_string(secrets, "athlete_id", settings.secrets.athlete_id) : error;
  error = error == ESP_OK ? put_string(secrets, "api_key", settings.secrets.api_key) : error;
  error = error == ESP_OK ? put_string(secrets, "admin_secret", settings.secrets.local_admin_secret) : error;
  return error == ESP_OK ? nvs_set_u32(secrets, "generation", settings.generation) : error;
}

bool read_slot(std::size_t slot, SettingsGeneration* loaded) {
  nvs_handle_t values = 0;
  nvs_handle_t secrets = 0;
  if (!open_slot(slot, NVS_READONLY, &values, &secrets)) return false;

  std::uint8_t complete = 0U;
  std::uint8_t ftp_override = 0U;
  std::uint8_t weight_override = 0U;
  std::uint32_t secret_generation = 0U;
  std::int32_t ftp_watts = 0;
  SettingsGeneration candidate;
  bool valid = nvs_get_u32(values, "schema", &candidate.schema_version) == ESP_OK &&
               nvs_get_u32(values, "generation", &candidate.generation) == ESP_OK &&
               nvs_get_u32(secrets, "generation", &secret_generation) == ESP_OK &&
               nvs_get_u8(values, "complete", &complete) == ESP_OK &&
               nvs_get_i32(values, "ftp", &ftp_watts) == ESP_OK &&
               get_blob(values, "rider_kg", &candidate.profile.rider_kg) == ESP_OK &&
               get_blob(values, "bike_kg", &candidate.profile.bike_kg) == ESP_OK &&
               get_blob(values, "crr", &candidate.profile.crr) == ESP_OK &&
               get_blob(values, "cda", &candidate.profile.cda) == ESP_OK &&
               nvs_get_u8(values, "ftp_override", &ftp_override) == ESP_OK &&
               nvs_get_u8(values, "weight_override", &weight_override) == ESP_OK &&
               nvs_get_u32(values, "tz_rule_ver", &candidate.profile.timezone_rule_version) == ESP_OK &&
               nvs_get_u32(values, "crc", &candidate.crc) == ESP_OK;
  if (valid) {
    valid = !read_string(values, "timezone", &candidate.profile.timezone, true, kMaxTimezoneBytes).error.has_value() &&
            !read_string(values, "tz_rule", &candidate.profile.timezone_rule, true, 96U).error.has_value() &&
            !read_string(secrets, "wifi_ssid", &candidate.secrets.wifi_ssid, true, kMaxWifiSsidBytes).error.has_value() &&
            !read_string(secrets, "wifi_password", &candidate.secrets.wifi_password, true, kMaxWifiPasswordBytes).error.has_value() &&
            !read_string(secrets, "athlete_id", &candidate.secrets.athlete_id, true, kMaxAthleteIdBytes).error.has_value() &&
            !read_string(secrets, "api_key", &candidate.secrets.api_key, true, kMaxApiKeyBytes).error.has_value();
    const Status admin = read_string(secrets, "admin_secret", &candidate.secrets.local_admin_secret, false, 128U);
    if (!admin) valid = false;
    if (!read_device(values, "trainer", PeripheralRole::kTrainer, &candidate.trainer_device) ||
        !read_device(values, "heart", PeripheralRole::kHeartRate, &candidate.heart_rate_device)) {
      valid = false;
    }
  }
  close_slot(values, secrets);
  if (!valid || secret_generation != candidate.generation || complete > 1U || ftp_override > 1U || weight_override > 1U) {
    return false;
  }
  candidate.profile.ftp_watts = static_cast<int>(ftp_watts);
  candidate.profile.ftp_local_override = ftp_override != 0U;
  candidate.profile.weight_local_override = weight_override != 0U;
  candidate.provisioning_complete = complete != 0U;
  *loaded = std::move(candidate);
  return true;
}

}  // namespace

Status NvsSettingsBackend::initialize() {
  const esp_err_t error = nvs_flash_init();
  if (error != ESP_OK) return Status::failure(ErrorCode::kStorage, "NVS initialization failed");
  return Status::ok();
}

bool NvsSettingsBackend::encrypted_at_rest() const {
#ifdef CONFIG_NVS_ENCRYPTION
  return true;
#else
  return false;
#endif
}

Status NvsSettingsBackend::save(const SettingsGeneration& settings) const {
  const Status profile = validate_profile(settings.profile);
  const Status secret = validate_secrets(settings.secrets);
  if (!profile || !secret || settings.crc != settings_crc(settings)) {
    return Status::failure(ErrorCode::kInvalidArgument, "settings failed validation before NVS write");
  }
  if (settings.schema_version != kSettingsSchemaVersion) {
    return Status::failure(ErrorCode::kUnsupported, "settings schema is unsupported");
  }

  nvs_handle_t values = 0;
  nvs_handle_t secrets = 0;
  const std::size_t slot = static_cast<std::size_t>(settings.generation & 1U);
  if (!open_slot(slot, NVS_READWRITE, &values, &secrets)) {
    return Status::failure(ErrorCode::kStorage, "NVS settings namespace unavailable");
  }
  const esp_err_t secret_error = write_secrets(secrets, settings);
  const esp_err_t value_error = secret_error == ESP_OK ? write_values(values, settings) : secret_error;
  esp_err_t error = value_error;
  if (error == ESP_OK) error = nvs_commit(secrets);
  if (error == ESP_OK) error = nvs_commit(values);
  close_slot(values, secrets);
  return error == ESP_OK ? Status::ok() : Status::failure(ErrorCode::kStorage, "NVS settings commit failed");
}

Status NvsSettingsBackend::load(SettingsStore* store) const {
  if (store == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "settings store is null");
  SettingsGeneration best;
  bool found = false;
  bool any_slot = false;
  for (std::size_t slot = 0U; slot < kValueNamespaces.size(); ++slot) {
    nvs_handle_t values = 0;
    nvs_handle_t secrets = 0;
    const bool namespaces_exist = nvs_open(kValueNamespaces[slot], NVS_READONLY, &values) == ESP_OK;
    if (namespaces_exist) any_slot = true;
    if (values != 0) nvs_close(values);
    if (nvs_open(kSecretNamespaces[slot], NVS_READONLY, &secrets) == ESP_OK) {
      any_slot = true;
      nvs_close(secrets);
    }
    SettingsGeneration candidate;
    if (!read_slot(slot, &candidate)) continue;
    if (!found || candidate.generation > best.generation) {
      best = std::move(candidate);
      found = true;
    }
  }
  if (!found) {
    return any_slot ? Status::failure(ErrorCode::kStorage, "no complete NVS settings generation") : Status::ok();
  }
  return store->restore(best);
}

}  // namespace trainer_loop

#else

namespace trainer_loop {

Status NvsSettingsBackend::initialize() { return Status::failure(ErrorCode::kUnsupported, "NVS requires ESP-IDF"); }
Status NvsSettingsBackend::load(SettingsStore*) const { return Status::failure(ErrorCode::kUnsupported, "NVS requires ESP-IDF"); }
Status NvsSettingsBackend::save(const SettingsGeneration&) const { return Status::failure(ErrorCode::kUnsupported, "NVS requires ESP-IDF"); }
bool NvsSettingsBackend::encrypted_at_rest() const { return false; }

}  // namespace trainer_loop

#endif
