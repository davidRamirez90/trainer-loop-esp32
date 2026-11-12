#include "platform_services.h"

#include <limits>
#include <algorithm>
#include <fstream>
#include <utility>

namespace trainer_loop {

HostPlatformServices& HostPlatformServices::instance() {
  static HostPlatformServices services;
  return services;
}

void HostPlatformServices::configure(std::filesystem::path root) {
  if (root.empty()) root = ".trainer-loop-host";
  root_ = std::move(root);
  std::error_code error;
  std::filesystem::create_directories(root_, error);
  const auto run_root = root_.filename() == "storage" ? root_.parent_path() : root_;
  std::filesystem::create_directories(run_root, error);
  record_event("host.configure");
}

void HostPlatformServices::set_monotonic_ms(std::uint64_t value) {
  monotonic_ms_ = value;
  process_due_events();
}

void HostPlatformServices::advance_monotonic_ms(std::uint64_t delta) {
  monotonic_ms_ = delta > std::numeric_limits<std::uint64_t>::max() - monotonic_ms_
                      ? std::numeric_limits<std::uint64_t>::max()
                      : monotonic_ms_ + delta;
  if (has_utc_) {
    const auto signed_delta = delta > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                                  ? std::numeric_limits<std::int64_t>::max()
                                  : static_cast<std::int64_t>(delta);
    utc_ms_ = signed_delta > 0 && utc_ms_ > std::numeric_limits<std::int64_t>::max() - signed_delta
                  ? std::numeric_limits<std::int64_t>::max()
                  : utc_ms_ + signed_delta;
  }
  process_due_events();
}

void HostPlatformServices::set_http_fixture(std::string method, std::string path,
                                            HttpFixture fixture) {
  http_fixtures_[std::move(method) + " " + std::move(path)] = std::move(fixture);
}

const HostPlatformServices::HttpFixture* HostPlatformServices::http_fixture(
    const std::string& method, const std::string& path) const {
  const auto found = http_fixtures_.find(method + " " + path);
  return found == http_fixtures_.end() ? nullptr : &found->second;
}

void HostPlatformServices::clear_http_fixtures() { http_fixtures_.clear(); }

void HostPlatformServices::record_event(std::string event) {
  constexpr std::size_t kMaxEvents = 256U;
  if (events_.size() == kMaxEvents) events_.erase(events_.begin());
  events_.push_back(event);
  const auto run_root = root_.filename() == "storage" ? root_.parent_path() : root_;
  std::ofstream stream(run_root / "platform-events.jsonl", std::ios::app);
  if (!stream) return;
  stream << "{\"t_ms\":" << monotonic_ms_ << ",\"event\":\"";
  for (const unsigned char c : event) {
    switch (c) {
      case '"': stream << "\\\""; break;
      case '\\': stream << "\\\\"; break;
      case '\n': stream << "\\n"; break;
      case '\r': stream << "\\r"; break;
      case '\t': stream << "\\t"; break;
      default:
        if (c < 0x20U) stream << '?';
        else stream << static_cast<char>(c);
        break;
    }
  }
  stream << "\"}\n";
}

bool HostPlatformServices::schedule_device_event(std::uint64_t at_ms, PeripheralRole role,
                                                  bool available, bool telemetry_enabled) {
  constexpr std::size_t kMaxScheduledEvents = 32U;
  if (scheduled_events_.size() >= kMaxScheduledEvents) return false;
  ScheduledEvent event;
  event.kind = ScheduledEvent::Kind::kDevice;
  event.at_ms = at_ms;
  event.role = role;
  event.available = available;
  event.telemetry_enabled = telemetry_enabled;
  scheduled_events_.push_back(event);
  std::stable_sort(scheduled_events_.begin(), scheduled_events_.end(),
                   [](const ScheduledEvent& left, const ScheduledEvent& right) {
                     return left.at_ms < right.at_ms;
                   });
  record_event("scheduled device at=" + std::to_string(at_ms) +
               " role=" + std::to_string(static_cast<unsigned>(role)));
  process_due_events();
  return true;
}

bool HostPlatformServices::schedule_network_event(std::uint64_t at_ms,
                                                   bool wifi_available,
                                                   bool http_available) {
  constexpr std::size_t kMaxScheduledEvents = 32U;
  if (scheduled_events_.size() >= kMaxScheduledEvents) return false;
  ScheduledEvent event;
  event.kind = ScheduledEvent::Kind::kNetwork;
  event.at_ms = at_ms;
  event.wifi_available = wifi_available;
  event.http_available = http_available;
  scheduled_events_.push_back(event);
  std::stable_sort(scheduled_events_.begin(), scheduled_events_.end(),
                   [](const ScheduledEvent& left, const ScheduledEvent& right) {
                     return left.at_ms < right.at_ms;
                   });
  record_event("scheduled network at=" + std::to_string(at_ms));
  process_due_events();
  return true;
}

std::optional<std::uint64_t> HostPlatformServices::next_scheduled_event_ms() const {
  if (scheduled_events_.empty()) return std::nullopt;
  return scheduled_events_.front().at_ms;
}

void HostPlatformServices::process_due_events() {
  while (!scheduled_events_.empty() && scheduled_events_.front().at_ms <= monotonic_ms_) {
    const ScheduledEvent event = scheduled_events_.front();
    scheduled_events_.erase(scheduled_events_.begin());
    if (event.kind == ScheduledEvent::Kind::kDevice) {
      if (event.role == PeripheralRole::kTrainer) {
        trainer_available_ = event.available;
        trainer_telemetry_enabled_ = event.telemetry_enabled;
      } else {
        heart_rate_available_ = event.available;
        heart_rate_telemetry_enabled_ = event.telemetry_enabled;
      }
      record_event("event device role=" + std::to_string(static_cast<unsigned>(event.role)) +
                   " available=" + (event.available ? "1" : "0") +
                   " telemetry=" + (event.telemetry_enabled ? "1" : "0") +
                   " t=" + std::to_string(event.at_ms));
    } else {
      wifi_available_ = event.wifi_available;
      http_available_ = event.http_available;
      record_event(std::string("event network wifi=") + (event.wifi_available ? "1" : "0") +
                   " http=" + (event.http_available ? "1" : "0") +
                   " t=" + std::to_string(event.at_ms));
    }
  }
}

}  // namespace trainer_loop
