#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace trainer_loop {

enum class PeripheralRole : std::uint8_t { kTrainer, kHeartRate };

struct DeviceIdentity {
  std::array<std::uint8_t, 6> address{};
  std::uint8_t address_type = 0U;
  std::string name;
  std::int8_t rssi = 0;
  PeripheralRole role = PeripheralRole::kTrainer;
};

struct ReconnectState {
  LinkState link = LinkState::kDisconnected;
  std::uint32_t generation = 0U;
  std::uint32_t failures = 0U;
  std::uint64_t next_attempt_ms = 0U;
  bool selected = false;
  bool services_ready = false;
};

inline bool notification_is_current(const ReconnectState& state, std::uint32_t generation) {
  return state.link == LinkState::kConnected && state.generation == generation;
}

std::uint32_t reconnect_delay_ms(std::uint32_t failure_count);
bool same_identity(const DeviceIdentity& left, const DeviceIdentity& right);

class BleConnectionManager {
 public:
  Status select(const DeviceIdentity& device);
  Status forget(PeripheralRole role);
  Status scan_started(std::uint64_t now_ms);
  Status poll_scan(std::uint64_t now_ms);
  Status scan_finished();
  Status connected(PeripheralRole role, std::uint64_t now_ms);
  Status disconnected(PeripheralRole role, std::uint64_t now_ms);
  Status services_ready(PeripheralRole role);
  Status mark_control_invalid(std::uint64_t now_ms);

  bool scanning() const { return scanning_; }
  const ReconnectState& state(PeripheralRole role) const;
  const std::optional<DeviceIdentity>& selected(PeripheralRole role) const;
  bool reconnect_due(PeripheralRole role, std::uint64_t now_ms) const;

 private:
  struct Link {
    std::optional<DeviceIdentity> selected;
    ReconnectState reconnect;
  } trainer_, heart_rate_;
  Link& link_for(PeripheralRole role);
  const Link& link_for(PeripheralRole role) const;
  bool scanning_ = false;
  std::uint64_t scan_deadline_ms_ = 0U;
};

}  // namespace trainer_loop
