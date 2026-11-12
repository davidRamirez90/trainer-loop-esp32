#include "settings_nvs.h"
#include "platform_services.h"
#include "trainer_loop/storage_io.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <limits>
#include <sstream>
#include <type_traits>

namespace trainer_loop {
namespace {
using Values = std::map<std::string, std::string>;

std::string encode_address(const DeviceIdentity& device) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(12U);
  for (const auto byte : device.address) {
    output.push_back(hex[byte >> 4U]);
    output.push_back(hex[byte & 0x0fU]);
  }
  return output;
}

bool decode_address(const std::string& encoded, DeviceIdentity* device) {
  if (device == nullptr || encoded.size() != 12U) return false;
  for (std::size_t i = 0U; i < device->address.size(); ++i) {
    unsigned value = 0U;
    const auto parsed = std::from_chars(encoded.data() + i * 2U, encoded.data() + i * 2U + 2U, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + i * 2U + 2U || value > 255U) return false;
    device->address[i] = static_cast<std::uint8_t>(value);
  }
  return true;
}

bool read_map(const std::filesystem::path& path, Values* values) {
  std::error_code error;
  if (std::filesystem::file_size(path, error) > 64U * 1024U || error) return false;
  std::ifstream input(path);
  if (!input) return false;
  std::string key;
  std::string value;
  while (input >> key >> std::quoted(value)) {
    if (!values->emplace(key, value).second) return false;
  }
  return input.eof();
}

Status write_map(const std::filesystem::path& path, const Values& values) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return Status::failure(ErrorCode::kStorage, "host NVS directory could not be created: " + error.message());
  const auto temporary = path.string() + ".tmp";
  {
    std::ostringstream buffer;
    for (const auto& entry : values) buffer << entry.first << ' ' << std::quoted(entry.second) << '\n';
    const std::string bytes = buffer.str();
    std::FILE* output = std::fopen(temporary.c_str(), "wb");
    if (output == nullptr) return Status::failure(ErrorCode::kStorage, "host NVS namespace could not be written");
    const std::size_t written = storage_write(bytes.data(), bytes.size(), output);
    const bool flushed = std::fflush(output) == 0;
    const bool closed = std::fclose(output) == 0;
    if (written != bytes.size() || !flushed || !closed) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return Status::failure(ErrorCode::kStorage, "host NVS namespace write failed");
    }
  }
  std::filesystem::rename(temporary, path, error);
  return error ? Status::failure(ErrorCode::kStorage, "host NVS namespace commit failed: " + error.message()) : Status::ok();
}

bool get(const Values& values, const char* key, std::string* result, bool required = true) {
  const auto found = values.find(key);
  if (found == values.end()) return !required;
  *result = found->second;
  return true;
}

template <typename T>
bool number(const Values& values, const char* key, T* result) {
  std::string text;
  if (!get(values, key, &text)) return false;
  if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
    unsigned long long parsed_value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), parsed_value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        parsed_value > static_cast<unsigned long long>(std::numeric_limits<T>::max())) return false;
    *result = static_cast<T>(parsed_value);
    return true;
  } else if constexpr (std::is_integral_v<T>) {
    long long parsed_value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), parsed_value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        parsed_value < static_cast<long long>(std::numeric_limits<T>::lowest()) ||
        parsed_value > static_cast<long long>(std::numeric_limits<T>::max())) return false;
    *result = static_cast<T>(parsed_value);
    return true;
  }
  std::istringstream input(text);
  input >> *result;
  return input && input.eof();
}

std::filesystem::path slot_path(bool secrets, std::size_t slot) {
  const char suffix = slot == 0U ? 'a' : 'b';
  const std::string name = secrets ? "trloop_sec_" : "trloop_val_";
  return HostPlatformServices::instance().root() / "nvs" / (name + suffix + ".nvs");
}

Values make_value_namespace(const SettingsGeneration& settings) {
  Values values;
  values["schema"] = std::to_string(settings.schema_version);
  values["generation"] = std::to_string(settings.generation);
  values["complete"] = settings.provisioning_complete ? "1" : "0";
  values["ftp"] = std::to_string(settings.profile.ftp_watts);
  auto put_double = [&values](const char* key, double value) { std::ostringstream stream; stream << std::setprecision(17) << value; values[key] = stream.str(); };
  put_double("rider_kg", settings.profile.rider_kg);
  put_double("bike_kg", settings.profile.bike_kg);
  put_double("crr", settings.profile.crr);
  put_double("cda", settings.profile.cda);
  values["ftp_override"] = settings.profile.ftp_local_override ? "1" : "0";
  values["weight_override"] = settings.profile.weight_local_override ? "1" : "0";
  values["tz_rule_ver"] = std::to_string(settings.profile.timezone_rule_version);
  values["crc"] = std::to_string(settings.crc);
  values["timezone"] = settings.profile.timezone;
  values["tz_rule"] = settings.profile.timezone_rule;
  const auto put_device = [&values](const char* prefix, const std::optional<DeviceIdentity>& device) {
    if (!device) return;
    values[std::string(prefix) + "_addr"] = encode_address(*device);
    values[std::string(prefix) + "_type"] = std::to_string(device->address_type);
    values[std::string(prefix) + "_name"] = device->name;
  };
  put_device("trainer", settings.trainer_device);
  put_device("heart", settings.heart_rate_device);
  return values;
}

Values make_secret_namespace(const SettingsGeneration& settings) {
  return {{"wifi_ssid", settings.secrets.wifi_ssid}, {"wifi_password", settings.secrets.wifi_password},
          {"athlete_id", settings.secrets.athlete_id}, {"api_key", settings.secrets.api_key},
          {"admin_secret", settings.secrets.local_admin_secret}, {"generation", std::to_string(settings.generation)}};
}

bool read_device(const Values& values, const char* prefix, PeripheralRole role,
                 std::optional<DeviceIdentity>* output) {
  const std::string address_key = std::string(prefix) + "_addr";
  const auto found = values.find(address_key);
  if (found == values.end()) { output->reset(); return true; }
  DeviceIdentity device;
  device.role = role;
  const std::string type_key = std::string(prefix) + "_type";
  if (!decode_address(found->second, &device) || !number(values, type_key.c_str(), &device.address_type) ||
      !get(values, (std::string(prefix) + "_name").c_str(), &device.name, false)) return false;
  *output = std::move(device);
  return true;
}

bool read_slot(std::size_t slot, SettingsGeneration* loaded) {
  Values values;
  Values secrets;
  if (!read_map(slot_path(false, slot), &values) || !read_map(slot_path(true, slot), &secrets)) return false;
  SettingsGeneration candidate;
  std::uint32_t secret_generation = 0U;
  int ftp = 0;
  int complete = 0;
  int ftp_override = 0;
  int weight_override = 0;
  if (!number(values, "schema", &candidate.schema_version) || !number(values, "generation", &candidate.generation) ||
      !number(secrets, "generation", &secret_generation) || !number(values, "complete", &complete) ||
      !number(values, "ftp", &ftp) || !number(values, "rider_kg", &candidate.profile.rider_kg) ||
      !number(values, "bike_kg", &candidate.profile.bike_kg) || !number(values, "crr", &candidate.profile.crr) ||
      !number(values, "cda", &candidate.profile.cda) || !number(values, "ftp_override", &ftp_override) ||
      !number(values, "weight_override", &weight_override) ||
      !number(values, "tz_rule_ver", &candidate.profile.timezone_rule_version) ||
      !number(values, "crc", &candidate.crc) || !get(values, "timezone", &candidate.profile.timezone) ||
      !get(values, "tz_rule", &candidate.profile.timezone_rule) || !get(secrets, "wifi_ssid", &candidate.secrets.wifi_ssid) ||
      !get(secrets, "wifi_password", &candidate.secrets.wifi_password) || !get(secrets, "athlete_id", &candidate.secrets.athlete_id) ||
      !get(secrets, "api_key", &candidate.secrets.api_key) || !get(secrets, "admin_secret", &candidate.secrets.local_admin_secret, false) ||
      !read_device(values, "trainer", PeripheralRole::kTrainer, &candidate.trainer_device) ||
      !read_device(values, "heart", PeripheralRole::kHeartRate, &candidate.heart_rate_device) ||
      secret_generation != candidate.generation || complete < 0 || complete > 1 || ftp_override < 0 || ftp_override > 1 ||
      weight_override < 0 || weight_override > 1) return false;
  candidate.profile.ftp_watts = ftp;
  candidate.profile.ftp_local_override = ftp_override != 0;
  candidate.profile.weight_local_override = weight_override != 0;
  candidate.provisioning_complete = complete != 0;
  if (candidate.crc != settings_crc(candidate)) return false;
  *loaded = std::move(candidate);
  return true;
}
}

Status NvsSettingsBackend::initialize() {
  std::error_code error;
  std::filesystem::create_directories(HostPlatformServices::instance().root() / "nvs", error);
  return error ? Status::failure(ErrorCode::kStorage, "host NVS directory could not be created: " + error.message()) : Status::ok();
}

Status NvsSettingsBackend::save(const SettingsGeneration& settings) const {
  if (settings.schema_version != kSettingsSchemaVersion || !validate_profile(settings.profile) ||
      !validate_secrets(settings.secrets) || settings.crc != settings_crc(settings))
    return Status::failure(ErrorCode::kInvalidArgument, "settings failed validation before host NVS write");
  const std::size_t slot = static_cast<std::size_t>(settings.generation & 1U);
  const Status secret_status = write_map(slot_path(true, slot), make_secret_namespace(settings));
  if (!secret_status) return secret_status;
  return write_map(slot_path(false, slot), make_value_namespace(settings));
}

Status NvsSettingsBackend::load(SettingsStore* store) const {
  if (store == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "settings store is null");
  SettingsGeneration best;
  bool found = false;
  bool any_slot = false;
  for (std::size_t slot = 0U; slot < 2U; ++slot) {
    any_slot = any_slot || std::filesystem::exists(slot_path(true, slot)) || std::filesystem::exists(slot_path(false, slot));
    SettingsGeneration candidate;
    if (read_slot(slot, &candidate) && (!found || candidate.generation > best.generation)) {
      best = std::move(candidate);
      found = true;
    }
  }
  if (!found) return any_slot ? Status::failure(ErrorCode::kStorage, "no complete host NVS settings generation") : Status::ok();
  return store->restore(best);
}

bool NvsSettingsBackend::encrypted_at_rest() const { return false; }

}  // namespace trainer_loop
