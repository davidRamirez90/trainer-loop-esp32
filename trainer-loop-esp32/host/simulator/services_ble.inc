#include "ble_nimble.h"
#include "platform_services.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace trainer_loop {

namespace {
constexpr std::size_t kQueueCapacity = 16U;
constexpr std::uint64_t kSyntheticTelemetryPeriodMs = 500U;
std::size_t role_index(PeripheralRole role) {
  return role == PeripheralRole::kTrainer ? 0U : 1U;
}
}

struct NimbleRuntime {
  bool host_ready = false;
  std::array<DeviceIdentity, 2U> candidates{};
  std::size_t candidate_count = 0U;
  std::array<BleNotification, kQueueCapacity> queue{};
  std::size_t head = 0U;
  std::size_t tail = 0U;
  std::size_t count = 0U;
  std::uint32_t dropped = 0U;
  std::array<ControlTransactionGate, 2U> gates{};
  TrainerCapabilities trainer_capabilities{};
  std::array<std::uint64_t, 2U> next_telemetry_ms{};
  std::uint32_t telemetry_tick = 0U;
  int target_power_w = 180;
  bool trainer_active = false;
  bool scan_complete = false;
  bool controller_wait_active = false;
};

namespace {
void queue_notification(NimbleRuntime* runtime, const BleNotification& notification) {
  if (runtime->count == runtime->queue.size()) {
    ++runtime->dropped;
    HostPlatformServices::instance().record_event("ble.notify.dropped");
    return;
  }
  runtime->queue[runtime->tail] = notification;
  runtime->tail = (runtime->tail + 1U) % runtime->queue.size();
  ++runtime->count;
  HostPlatformServices::instance().record_event(
      "ble.notify role=" + std::to_string(static_cast<unsigned>(notification.role)) +
      " kind=" + std::to_string(static_cast<unsigned>(notification.kind)) +
      " gen=" + std::to_string(notification.generation) +
      " t=" + std::to_string(notification.timestamp_ms));
}

BleNotification make_notification(PeripheralRole role, BleNotificationKind kind,
                                  std::uint32_t generation, std::uint64_t now_ms,
                                  const std::uint8_t* payload, std::size_t length,
                                  bool indication = false) {
  BleNotification result;
  result.role = role;
  result.kind = kind;
  result.generation = generation;
  result.length = static_cast<std::uint8_t>(std::min<std::size_t>(length, result.payload.size()));
  result.timestamp_ms = now_ms;
  result.indication = indication;
  std::copy_n(payload, result.length, result.payload.begin());
  return result;
}

Status disconnect_simulated(NimbleCentral* central, NimbleRuntime* runtime,
                            PeripheralRole role, std::uint64_t now_ms) {
  auto& gate = runtime->gates[role_index(role)];
  gate.invalidate();
  const Status disconnected = central->connection_manager().state(role).link == LinkState::kConnected
      ? const_cast<BleConnectionManager&>(central->connection_manager()).disconnected(role, now_ms)
      : Status::ok();
  if (!disconnected) return disconnected;
  HostPlatformServices::instance().record_event(
      "ble.disconnect role=" + std::to_string(static_cast<unsigned>(role)) +
      " gen=" + std::to_string(central->connection_manager().state(role).generation) +
      " t=" + std::to_string(now_ms));
  runtime->next_telemetry_ms[role_index(role)] = 0U;
  return Status::ok();
}

void queue_telemetry(NimbleCentral* central, NimbleRuntime* runtime, std::uint64_t now_ms) {
  const auto& manager = central->connection_manager();
  const auto& trainer = manager.state(PeripheralRole::kTrainer);
  if (trainer.link == LinkState::kConnected && trainer.services_ready &&
      HostPlatformServices::instance().trainer_telemetry_enabled() &&
      now_ms >= runtime->next_telemetry_ms[0]) {
    runtime->next_telemetry_ms[0] = now_ms + kSyntheticTelemetryPeriodMs;
    ++runtime->telemetry_tick;
    const int power = runtime->trainer_active
        ? runtime->target_power_w + static_cast<int>(runtime->telemetry_tick % 9U) - 4
        : 0;
    const std::uint16_t speed = static_cast<std::uint16_t>(2600U + runtime->telemetry_tick % 300U);
    const std::uint16_t cadence = runtime->trainer_active
        ? static_cast<std::uint16_t>((85U + runtime->telemetry_tick % 5U) * 2U) : 0U;
    const std::uint32_t distance = runtime->telemetry_tick * 3U;
    const std::uint16_t flags = 0x0054U; // cadence, total distance, instantaneous power
    const std::array<std::uint8_t, 11U> payload = {
        static_cast<std::uint8_t>(flags), static_cast<std::uint8_t>(flags >> 8U),
        static_cast<std::uint8_t>(speed), static_cast<std::uint8_t>(speed >> 8U),
        static_cast<std::uint8_t>(cadence), static_cast<std::uint8_t>(cadence >> 8U),
        static_cast<std::uint8_t>(distance), static_cast<std::uint8_t>(distance >> 8U),
        static_cast<std::uint8_t>(distance >> 16U), static_cast<std::uint8_t>(power),
        static_cast<std::uint8_t>(static_cast<std::uint16_t>(power) >> 8U)};
    queue_notification(runtime, make_notification(PeripheralRole::kTrainer,
        BleNotificationKind::kIndoorBikeData, trainer.generation, now_ms,
        payload.data(), payload.size()));
  }
  const auto& heart = manager.state(PeripheralRole::kHeartRate);
  if (heart.link == LinkState::kConnected && heart.services_ready &&
      HostPlatformServices::instance().heart_rate_telemetry_enabled() &&
      now_ms >= runtime->next_telemetry_ms[1]) {
    runtime->next_telemetry_ms[1] = now_ms + 1000U;
    const std::array<std::uint8_t, 2U> payload = {
        0x06U, static_cast<std::uint8_t>(132U + runtime->telemetry_tick % 12U)};
    queue_notification(runtime, make_notification(PeripheralRole::kHeartRate,
        BleNotificationKind::kHeartRateMeasurement, heart.generation, now_ms,
        payload.data(), payload.size()));
  }
}

void advance_with_service_pump(NimbleCentral* central, NimbleRuntime* runtime,
                               std::uint32_t duration_ms,
                               const std::function<void(std::uint64_t)>& observer) {
  auto& services = HostPlatformServices::instance();
  std::uint32_t elapsed = 0U;
  runtime->controller_wait_active = true;
  while (elapsed < duration_ms) {
    services.process_due_events();
    std::uint64_t step64 = std::min<std::uint32_t>(100U, duration_ms - elapsed);
    if (const auto deadline = services.next_scheduled_event_ms()) {
      const auto now = services.monotonic_ms();
      if (*deadline <= now) {
        services.process_due_events();
        continue;
      }
      step64 = std::min(step64, *deadline - now);
    }
    const auto step = static_cast<std::uint32_t>(step64);
    services.advance_monotonic_ms(step);
    elapsed += step;
    const Status polled = central->poll(services.monotonic_ms());
    if (!polled) services.record_event("ble.wait.poll-failed");
    if (observer) observer(services.monotonic_ms());
  }
  runtime->controller_wait_active = false;
}
}

NimbleCentral::NimbleCentral() = default;
NimbleCentral::~NimbleCentral() = default;

Status NimbleCentral::initialize() {
  if (!runtime_) runtime_ = std::make_unique<NimbleRuntime>();
  runtime_->host_ready = true;
  initialized_ = true;
  return Status::ok();
}

Status NimbleCentral::start_foreground_scan(std::uint64_t now_ms) {
  if (!initialized_ || !runtime_) return Status::failure(ErrorCode::kConflict, "BLE host is not initialized");
  if (scan_active_) return Status::failure(ErrorCode::kConflict, "BLE scan is already active");
  const Status started = connections_.scan_started(now_ms);
  if (!started) return started;
  HostPlatformServices::instance().record_event("ble.scan.start t=" + std::to_string(now_ms));
  runtime_->candidates[0] = DeviceIdentity{{0xA4U,0xC1U,0x38U,0x10U,0x00U,0x01U}, 0U,
      "Trainer Loop FTMS", -38, PeripheralRole::kTrainer};
  runtime_->candidates[1] = DeviceIdentity{{0xA4U,0xC1U,0x38U,0x10U,0x00U,0x02U}, 0U,
      "Trainer Loop HR", -44, PeripheralRole::kHeartRate};
  runtime_->candidate_count = runtime_->candidates.size();
  scan_active_ = true;
  runtime_->scan_complete = true;
  return Status::ok();
}

Status NimbleCentral::poll_scan(std::uint64_t now_ms) { return poll(now_ms); }

Status NimbleCentral::poll(std::uint64_t now_ms) {
  if (!initialized_ || !runtime_) return Status::failure(ErrorCode::kConflict, "BLE host is not initialized");
  if (scan_active_) {
    scan_active_ = false;
    runtime_->scan_complete = false;
    const Status finished = connections_.scan_finished();
    if (!finished) return finished;
  }
  const bool transaction_pending = runtime_->controller_wait_active || runtime_->gates[0].pending();
  for (const PeripheralRole role : {PeripheralRole::kTrainer, PeripheralRole::kHeartRate}) {
    const auto& state = connections_.state(role);
    const bool available = role == PeripheralRole::kTrainer
        ? HostPlatformServices::instance().trainer_available()
        : HostPlatformServices::instance().heart_rate_available();
    if (state.link == LinkState::kConnected && !available) {
      const Status disconnected = disconnect_simulated(this, runtime_.get(), role, now_ms);
      if (!disconnected) return disconnected;
      continue;
    }
    if (!available) continue;
    if (transaction_pending) continue;
    if (state.selected && state.link != LinkState::kConnected &&
        (state.link == LinkState::kDisconnected || connections_.reconnect_due(role, now_ms))) {
      if (!HostPlatformServices::instance().ftms_behavior().auto_reconnect &&
          state.link != LinkState::kDisconnected) continue;
      const Status connected = connect_selected(role);
      if (!connected && connected.error->code != ErrorCode::kConflict) return connected;
    }
  }
  queue_telemetry(this, runtime_.get(), now_ms);
  return Status::ok();
}

Status NimbleCentral::stop_scan() {
  scan_active_ = false;
  if (runtime_) runtime_->scan_complete = false;
  return connections_.scan_finished();
}

std::size_t NimbleCentral::scan_result_count() const {
  return runtime_ ? runtime_->candidate_count : 0U;
}

bool NimbleCentral::scan_result(std::size_t index, DeviceIdentity* result) const {
  if (!runtime_ || result == nullptr || index >= runtime_->candidate_count) return false;
  *result = runtime_->candidates[index];
  return true;
}

Status NimbleCentral::select_device(const DeviceIdentity& device) {
  if (!initialized_ || !runtime_) return Status::failure(ErrorCode::kConflict, "BLE host is not initialized");
  if (device.name.empty() || device.address_type > 1U) return Status::failure(ErrorCode::kInvalidArgument, "BLE identity is invalid");
  if (scan_active_) (void)stop_scan();
  const Status selected = connections_.select(device);
  if (!selected) return selected;
  runtime_->gates[role_index(device.role)].invalidate();
  runtime_->next_telemetry_ms[role_index(device.role)] = 0U;
  return connect_selected(device.role);
}

Status NimbleCentral::forget_device(PeripheralRole role) {
  if (!initialized_ || !runtime_) return Status::failure(ErrorCode::kConflict, "BLE host is not initialized");
  runtime_->gates[role_index(role)].invalidate();
  runtime_->next_telemetry_ms[role_index(role)] = 0U;
  return connections_.forget(role);
}

Status NimbleCentral::connect_selected(PeripheralRole role) {
  if (!initialized_ || !runtime_) return Status::failure(ErrorCode::kConflict, "BLE host is not initialized");
  if (!connections_.selected(role)) return Status::failure(ErrorCode::kConflict, "no BLE device is selected");
  if ((role == PeripheralRole::kTrainer && !HostPlatformServices::instance().trainer_available()) ||
      (role == PeripheralRole::kHeartRate && !HostPlatformServices::instance().heart_rate_available()))
    return Status::failure(ErrorCode::kTransport, "simulated BLE peripheral is unavailable");
  if (connections_.state(role).link == LinkState::kConnected) return Status::ok();
  const Status connected = connections_.connected(role, HostPlatformServices::instance().monotonic_ms());
  if (!connected) return connected;
  HostPlatformServices::instance().record_event(
      "ble.connect role=" + std::to_string(static_cast<unsigned>(role)) +
      " gen=" + std::to_string(connections_.state(role).generation) +
      " t=" + std::to_string(HostPlatformServices::instance().monotonic_ms()));
  const Status ready = connections_.services_ready(role);
  if (!ready) return ready;
  if (role == PeripheralRole::kTrainer) {
    runtime_->trainer_capabilities = HostPlatformServices::instance().trainer_capabilities();
  }
  runtime_->next_telemetry_ms[role_index(role)] = HostPlatformServices::instance().monotonic_ms();
  return Status::ok();
}

Status NimbleCentral::write_ftms_command(FtmsOpcode opcode, const std::uint8_t* payload,
                                         std::size_t length, std::uint64_t now_ms) {
  if (!initialized_ || !runtime_ || !trainer_services_ready()) return Status::failure(ErrorCode::kConflict, "FTMS services are not ready");
  if (payload == nullptr || length == 0U || length > kBleNotificationMaxBytes || payload[0] != static_cast<std::uint8_t>(opcode))
    return Status::failure(ErrorCode::kInvalidArgument, "FTMS command payload does not match opcode");
  if (opcode != FtmsOpcode::kRequestControl && !trainer_control_valid())
    return Status::failure(ErrorCode::kConflict, "FTMS control is not acknowledged and ready");
  const std::uint32_t generation = connections_.state(PeripheralRole::kTrainer).generation;
  auto& gate = runtime_->gates[0];
  const Status begun = gate.begin(opcode, generation, now_ms);
  if (!begun) return begun;
  auto& behavior = HostPlatformServices::instance().ftms_behavior();
  if (behavior.timeout_next_command) {
    behavior.timeout_next_command = false;
    HostPlatformServices::instance().record_event("ftms.timeout opcode=" + std::to_string(static_cast<unsigned>(opcode)) +
                                                   " gen=" + std::to_string(generation));
    advance_with_service_pump(this, runtime_.get(), 3001U, wait_observer_);
    (void)gate.poll_timeout(HostPlatformServices::instance().monotonic_ms());
    (void)connections_.mark_control_invalid(HostPlatformServices::instance().monotonic_ms());
    return Status::failure(ErrorCode::kTimeout, "FTMS acknowledgement timed out");
  }
  if (behavior.disconnect_before_next_ack) {
    behavior.disconnect_before_next_ack = false;
    HostPlatformServices::instance().record_event("ftms.disconnect-before-ack gen=" + std::to_string(generation));
    (void)disconnect_simulated(this, runtime_.get(), PeripheralRole::kTrainer, now_ms);
    return Status::failure(ErrorCode::kTransport, "trainer disconnected before FTMS acknowledgement");
  }
  if (behavior.acknowledgement_delay_ms > 3000U) {
    advance_with_service_pump(this, runtime_.get(), 3001U, wait_observer_);
    HostPlatformServices::instance().record_event("ftms.timeout opcode=" + std::to_string(static_cast<unsigned>(opcode)) +
                                                   " gen=" + std::to_string(generation));
    (void)gate.poll_timeout(HostPlatformServices::instance().monotonic_ms());
    (void)connections_.mark_control_invalid(HostPlatformServices::instance().monotonic_ms());
    return Status::failure(ErrorCode::kTimeout, "FTMS acknowledgement timed out");
  }
  advance_with_service_pump(this, runtime_.get(), behavior.acknowledgement_delay_ms, wait_observer_);
  const std::uint8_t result_code = behavior.reject_next_command
      ? static_cast<std::uint8_t>(FtmsResultCode::kOperationFailed)
      : static_cast<std::uint8_t>(FtmsResultCode::kSuccess);
  behavior.reject_next_command = false;
  const std::array<std::uint8_t, 3U> response = {0x80U, static_cast<std::uint8_t>(opcode), result_code};
  const std::uint32_t ack_generation = behavior.emit_stale_generation_next && generation > 0U
      ? generation - 1U : generation;
  behavior.emit_stale_generation_next = false;
  const auto ack = gate.acknowledge(response.data(), response.size(), ack_generation,
                                    HostPlatformServices::instance().monotonic_ms());
  queue_notification(runtime_.get(), make_notification(PeripheralRole::kTrainer,
      BleNotificationKind::kFitnessMachineControlPoint, ack_generation,
      HostPlatformServices::instance().monotonic_ms(), response.data(), response.size(), true));
  if (!ack) {
    HostPlatformServices::instance().record_event(
        "ftms.ack-rejected opcode=" + std::to_string(static_cast<unsigned>(opcode)) +
        " ack_gen=" + std::to_string(ack_generation) + " link_gen=" + std::to_string(generation) +
        " t=" + std::to_string(HostPlatformServices::instance().monotonic_ms()));
    gate.invalidate();
    return Status::failure(ack.error->code, ack.error->message);
  }
  HostPlatformServices::instance().record_event(
      "ftms.ack opcode=" + std::to_string(static_cast<unsigned>(opcode)) +
      " gen=" + std::to_string(ack_generation) +
      " t=" + std::to_string(HostPlatformServices::instance().monotonic_ms()));
  if (opcode == FtmsOpcode::kStartResume) runtime_->trainer_active = true;
  if (opcode == FtmsOpcode::kStopPause) runtime_->trainer_active = length < 2U || payload[1] == static_cast<std::uint8_t>(FtmsStopParameter::kStop) ? false : runtime_->trainer_active;
  if (opcode == FtmsOpcode::kSetTargetPower && length >= 3U)
    runtime_->target_power_w = static_cast<std::int16_t>(payload[1] | (static_cast<std::uint16_t>(payload[2]) << 8U));
  return Status::ok();
}

Status NimbleCentral::send_and_wait(FtmsOpcode opcode, const std::uint8_t* payload, std::size_t length) {
  return write_ftms_command(opcode, payload, length, HostPlatformServices::instance().monotonic_ms());
}
Status NimbleCentral::acquire_control() { const auto bytes = encode_request_control(); return send_and_wait(FtmsOpcode::kRequestControl, bytes.data(), bytes.size()); }
Status NimbleCentral::set_target_power(int watts) { const auto bytes = encode_target_power(watts, trainer_capabilities()); if (!bytes) return Status::failure(bytes.error->code, bytes.error->message); return send_and_wait(FtmsOpcode::kSetTargetPower, bytes.value->data(), bytes.value->size()); }
Status NimbleCentral::start_or_resume() { const auto bytes = encode_start_resume(); return send_and_wait(FtmsOpcode::kStartResume, bytes.data(), bytes.size()); }
Status NimbleCentral::pause() { const auto bytes = encode_stop_pause(FtmsStopParameter::kPause); return send_and_wait(FtmsOpcode::kStopPause, bytes.data(), bytes.size()); }
Status NimbleCentral::stop() { const auto bytes = encode_stop_pause(FtmsStopParameter::kStop); return send_and_wait(FtmsOpcode::kStopPause, bytes.data(), bytes.size()); }

bool NimbleCentral::pop_notification(BleNotification* notification) {
  if (!runtime_ || notification == nullptr || runtime_->count == 0U) return false;
  *notification = runtime_->queue[runtime_->head];
  runtime_->head = (runtime_->head + 1U) % runtime_->queue.size();
  --runtime_->count;
  return true;
}
std::uint32_t NimbleCentral::dropped_notification_count() const { return runtime_ ? runtime_->dropped : 0U; }
bool NimbleCentral::host_ready() const { return runtime_ && runtime_->host_ready; }
bool NimbleCentral::trainer_services_ready() const { return connections_.state(PeripheralRole::kTrainer).services_ready; }
bool NimbleCentral::heart_rate_services_ready() const { return connections_.state(PeripheralRole::kHeartRate).services_ready; }
bool NimbleCentral::trainer_control_valid() const { return runtime_ && runtime_->gates[0].control_valid(); }
bool NimbleCentral::trainer_control_pending() const { return runtime_ && runtime_->gates[0].pending(); }
TrainerCapabilities NimbleCentral::trainer_capabilities() const {
  if (runtime_ && trainer_services_ready()) return HostPlatformServices::instance().trainer_capabilities();
  return runtime_ ? runtime_->trainer_capabilities : TrainerCapabilities{};
}

Status NimbleCentral::host_inject_notification(const BleNotification& notification) {
  if (!runtime_ || !initialized_) return Status::failure(ErrorCode::kConflict, "BLE host is not initialized");
  if (notification.length > kBleNotificationMaxBytes) return Status::failure(ErrorCode::kBounds, "notification payload exceeds the host queue bound");
  if (notification.timestamp_ms > HostPlatformServices::instance().monotonic_ms())
    return Status::failure(ErrorCode::kInvalidArgument, "notification timestamp is in the future");
  queue_notification(runtime_.get(), notification);
  return Status::ok();
}

}  // namespace trainer_loop
