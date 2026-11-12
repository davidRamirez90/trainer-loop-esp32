#pragma once

#include "trainer_loop/ble_manager.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trainer_loop {

// Deterministic controls shared by the host implementations of the firmware
// platform adapters. No host service opens a live radio or network socket.
struct HostFtmsBehavior {
  std::uint32_t acknowledgement_delay_ms = 40U;
  bool reject_next_command = false;
  bool timeout_next_command = false;
  bool disconnect_before_next_ack = false;
  bool emit_stale_generation_next = false;
  bool auto_reconnect = true;
};

class HostPlatformServices {
 public:
  static HostPlatformServices& instance();

  void configure(std::filesystem::path root);
  const std::filesystem::path& root() const { return root_; }
  void set_monotonic_ms(std::uint64_t value);
  void advance_monotonic_ms(std::uint64_t delta);
  std::uint64_t monotonic_ms() const { return monotonic_ms_; }
  void set_utc_ms(std::int64_t value) { utc_ms_ = value; has_utc_ = true; }
  bool has_utc() const { return has_utc_; }
  std::int64_t utc_ms() const { return utc_ms_; }
  void set_boot_pressed(bool value) { boot_pressed_ = value; }
  bool boot_pressed() const { return boot_pressed_; }
  HostFtmsBehavior& ftms_behavior() { return ftms_behavior_; }
  const HostFtmsBehavior& ftms_behavior() const { return ftms_behavior_; }

  // Optional local HTTP fixtures are keyed by method + path. Unconfigured
  // Intervals endpoints use a bounded synthetic fixture response.
  struct HttpFixture {
    int status = 200;
    std::string body;
    std::uint32_t retry_after_s = 0U;
  };
  void set_http_fixture(std::string method, std::string path, HttpFixture fixture);
  const HttpFixture* http_fixture(const std::string& method,
                                  const std::string& path) const;
  void clear_http_fixtures();

  bool wifi_available() const { return wifi_available_; }
  void set_wifi_available(bool value) { wifi_available_ = value; }
  bool time_sync_available() const { return time_sync_available_; }
  void set_time_sync_available(bool value) { time_sync_available_ = value; }
  bool http_available() const { return http_available_; }
  void set_http_available(bool value) { http_available_ = value; }
  bool trainer_available() const { return trainer_available_; }
  void set_trainer_available(bool value) { trainer_available_ = value; }
  bool heart_rate_available() const { return heart_rate_available_; }
  void set_heart_rate_available(bool value) { heart_rate_available_ = value; }
  bool trainer_telemetry_enabled() const { return trainer_telemetry_enabled_; }
  void set_trainer_telemetry_enabled(bool value) { trainer_telemetry_enabled_ = value; }
  bool heart_rate_telemetry_enabled() const { return heart_rate_telemetry_enabled_; }
  void set_heart_rate_telemetry_enabled(bool value) { heart_rate_telemetry_enabled_ = value; }
  const TrainerCapabilities& trainer_capabilities() const { return trainer_capabilities_; }
  void set_trainer_capabilities(TrainerCapabilities value) { trainer_capabilities_ = value; }
  void record_event(std::string event);
  const std::vector<std::string>& events() const { return events_; }
  void clear_events() { events_.clear(); }
  bool schedule_device_event(std::uint64_t at_ms, PeripheralRole role,
                             bool available, bool telemetry_enabled);
  bool schedule_network_event(std::uint64_t at_ms, bool wifi_available,
                             bool http_available);
  // Absolute deadline of the next queued event. Events at or before the
  // current clock are consumed synchronously by scheduling/clock updates.
  std::optional<std::uint64_t> next_scheduled_event_ms() const;
  void clear_scheduled_events() { scheduled_events_.clear(); }
  void process_due_events();

 private:
  HostPlatformServices() = default;
  std::filesystem::path root_ = ".trainer-loop-host";
  std::uint64_t monotonic_ms_ = 0U;
  std::int64_t utc_ms_ = 0;
  bool has_utc_ = false;
  bool boot_pressed_ = false;
  bool wifi_available_ = true;
  bool time_sync_available_ = true;
  bool http_available_ = true;
  bool trainer_available_ = true;
  bool heart_rate_available_ = true;
  bool trainer_telemetry_enabled_ = true;
  bool heart_rate_telemetry_enabled_ = true;
  TrainerCapabilities trainer_capabilities_{true, true, true, true, true, 50, 1000, 1};
  HostFtmsBehavior ftms_behavior_;
  std::unordered_map<std::string, HttpFixture> http_fixtures_;
  std::vector<std::string> events_;
  struct ScheduledEvent {
    enum class Kind : std::uint8_t { kDevice, kNetwork } kind = Kind::kDevice;
    std::uint64_t at_ms = 0U;
    PeripheralRole role = PeripheralRole::kTrainer;
    bool available = true;
    bool telemetry_enabled = true;
    bool wifi_available = true;
    bool http_available = true;
  };
  std::vector<ScheduledEvent> scheduled_events_;
};

}  // namespace trainer_loop
