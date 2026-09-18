#include "trainer_loop/ble_manager.h"

#include <algorithm>

namespace trainer_loop {

namespace {

bool increment_generation(ReconnectState* state) {
  if (state == nullptr || state->generation == UINT32_MAX) return false;
  ++state->generation;
  return true;
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
  return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

}  // namespace

std::uint32_t reconnect_delay_ms(std::uint32_t failure_count) {
  static constexpr std::uint32_t kDelays[] = {1000U, 2000U, 4000U, 8000U, 15000U, 30000U};
  return kDelays[std::min<std::size_t>(failure_count, 5U)];
}

bool same_identity(const DeviceIdentity& left, const DeviceIdentity& right) {
  return left.address == right.address && left.address_type == right.address_type && left.role == right.role;
}

BleConnectionManager::Link& BleConnectionManager::link_for(PeripheralRole role) {
  return role == PeripheralRole::kTrainer ? trainer_ : heart_rate_;
}

const BleConnectionManager::Link& BleConnectionManager::link_for(PeripheralRole role) const {
  return role == PeripheralRole::kTrainer ? trainer_ : heart_rate_;
}

Status BleConnectionManager::select(const DeviceIdentity& device) {
  Link* link = &link_for(device.role);
  // An explicit selection is a new ownership epoch even when the address is
  // unchanged. This invalidates callbacks from a connection the user just
  // replaced or re-entered.
  if (!increment_generation(&link->reconnect)) {
    return Status::failure(ErrorCode::kOverflow, "BLE connection generation is exhausted");
  }
  link->selected = device;
  link->reconnect.selected = true;
  link->reconnect.link = LinkState::kDisconnected;
  link->reconnect.failures = 0U;
  link->reconnect.services_ready = false;
  // Selecting a result completes the foreground discovery interaction. The
  // ESP32 transport cancels the controller scan; keep the portable state
  // model in sync so the UI can immediately start another scan.
  (void)scan_finished();
  return Status::ok();
}

Status BleConnectionManager::forget(PeripheralRole role) {
  Link* link = &link_for(role);
  if (link->reconnect.generation == UINT32_MAX) {
    return Status::failure(ErrorCode::kOverflow, "BLE connection generation is exhausted");
  }
  const std::uint32_t next_generation = link->reconnect.generation + 1U;
  link->selected.reset();
  link->reconnect = ReconnectState{};
  link->reconnect.generation = next_generation;
  return Status::ok();
}

Status BleConnectionManager::scan_started(std::uint64_t now_ms) {
  if (scanning_) return Status::failure(ErrorCode::kConflict, "BLE scan is already running");
  scanning_ = true;
  scan_deadline_ms_ = saturating_add(now_ms, 15000U);
  return Status::ok();
}

Status BleConnectionManager::poll_scan(std::uint64_t now_ms) {
  if (scanning_ && now_ms >= scan_deadline_ms_) return scan_finished();
  return Status::ok();
}

Status BleConnectionManager::scan_finished() {
  scanning_ = false;
  scan_deadline_ms_ = 0U;
  return Status::ok();
}

bool BleConnectionManager::network_sync_blocked() const {
  if (scanning_) return true;
  const auto discovery_active = [](const Link& link) {
    return link.selected.has_value() && link.reconnect.link == LinkState::kConnected &&
           !link.reconnect.services_ready;
  };
  return discovery_active(trainer_) || discovery_active(heart_rate_);
}

Status BleConnectionManager::connected(PeripheralRole role, std::uint64_t now_ms) {
  Link* link = &link_for(role);
  if (!link->selected.has_value()) return Status::failure(ErrorCode::kConflict, "unselected BLE device connected");
  if (!increment_generation(&link->reconnect)) {
    return Status::failure(ErrorCode::kOverflow, "BLE connection generation is exhausted");
  }
  link->reconnect.link = LinkState::kConnected;
  link->reconnect.failures = 0U;
  link->reconnect.next_attempt_ms = now_ms;
  link->reconnect.services_ready = false;
  return Status::ok();
}

Status BleConnectionManager::disconnected(PeripheralRole role, std::uint64_t now_ms) {
  Link* link = &link_for(role);
  if (!link->selected.has_value()) return Status::failure(ErrorCode::kConflict, "unselected BLE device disconnected");
  if (!increment_generation(&link->reconnect)) {
    return Status::failure(ErrorCode::kOverflow, "BLE connection generation is exhausted");
  }
  link->reconnect.link = LinkState::kReconnecting;
  link->reconnect.services_ready = false;
  link->reconnect.next_attempt_ms = saturating_add(now_ms, reconnect_delay_ms(link->reconnect.failures));
  if (link->reconnect.failures < UINT32_MAX) ++link->reconnect.failures;
  return Status::ok();
}

Status BleConnectionManager::services_ready(PeripheralRole role) {
  Link* link = &link_for(role);
  if (link->reconnect.link != LinkState::kConnected) return Status::failure(ErrorCode::kConflict, "BLE link is not connected");
  link->reconnect.services_ready = true;
  return Status::ok();
}

Status BleConnectionManager::mark_control_invalid(std::uint64_t now_ms) {
  if (!trainer_.selected.has_value()) return Status::failure(ErrorCode::kConflict, "trainer is not selected");
  if (!increment_generation(&trainer_.reconnect)) {
    return Status::failure(ErrorCode::kOverflow, "BLE connection generation is exhausted");
  }
  trainer_.reconnect.services_ready = false;
  trainer_.reconnect.link = LinkState::kReconnecting;
  trainer_.reconnect.next_attempt_ms = saturating_add(now_ms, reconnect_delay_ms(trainer_.reconnect.failures));
  if (trainer_.reconnect.failures < UINT32_MAX) ++trainer_.reconnect.failures;
  return Status::ok();
}

const ReconnectState& BleConnectionManager::state(PeripheralRole role) const {
  return role == PeripheralRole::kTrainer ? trainer_.reconnect : heart_rate_.reconnect;
}

const std::optional<DeviceIdentity>& BleConnectionManager::selected(PeripheralRole role) const {
  return role == PeripheralRole::kTrainer ? trainer_.selected : heart_rate_.selected;
}

bool BleConnectionManager::reconnect_due(PeripheralRole role, std::uint64_t now_ms) const {
  const ReconnectState& current = state(role);
  return current.link == LinkState::kReconnecting && current.next_attempt_ms <= now_ms;
}

}  // namespace trainer_loop
