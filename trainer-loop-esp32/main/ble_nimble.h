#pragma once

#include "trainer_loop/ble_manager.h"
#include "trainer_loop/ble_protocol.h"
#include "trainer_loop/state_machine.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace trainer_loop {

// NimBLE callbacks run on the host task.  They copy only bounded values into
// this queue; parsing and workout-state changes remain in the coordinator
// task.  A notification is never allowed to block the radio callback.
constexpr std::size_t kBleNotificationMaxBytes = 64U;

enum class BleNotificationKind : std::uint8_t {
  kIndoorBikeData,
  kFitnessMachineStatus,
  kFitnessMachineControlPoint,
  kHeartRateMeasurement,
};

struct BleNotification {
  PeripheralRole role = PeripheralRole::kTrainer;
  BleNotificationKind kind = BleNotificationKind::kIndoorBikeData;
  std::uint32_t generation = 0U;
  std::uint16_t attribute_handle = 0U;
  std::uint8_t length = 0U;
  bool indication = false;
  std::uint64_t timestamp_ms = 0U;
  std::array<std::uint8_t, kBleNotificationMaxBytes> payload{};
};

struct NimbleRuntime;

class NimbleCentral : public ITrainerController {
 public:
  NimbleCentral();
  ~NimbleCentral();

  Status initialize();
  Status start_foreground_scan(std::uint64_t now_ms);
  Status poll(std::uint64_t now_ms);
  Status poll_scan(std::uint64_t now_ms);
  Status stop_scan();

  // Discovery results are data only.  No candidate is selected by name,
  // RSSI, advertisement order, or service appearance alone.
  std::size_t scan_result_count() const;
  bool scan_result(std::size_t index, DeviceIdentity* result) const;

  // Selection is an explicit user/coordinator action.  The address type is
  // retained because a BLE address without its type is not an identity.
  Status select_device(const DeviceIdentity& device);
  Status forget_device(PeripheralRole role);
  Status connect_selected(PeripheralRole role);

  // The ATT write callback only reports transport completion.  The command is
  // successful only after a matching FTMS indication reaches the gate.
  Status write_ftms_command(FtmsOpcode opcode, const std::uint8_t* payload,
                           std::size_t length, std::uint64_t now_ms);

  // ITrainerController bridge used by SessionCoordinator.  Each method
  // returns only after the matching FTMS indication has been observed or the
  // bounded transaction has failed.
  Status acquire_control() override;
  Status set_target_power(int watts) override;
  Status start_or_resume() override;
  Status pause() override;
  Status stop() override;

  bool pop_notification(BleNotification* notification);
  std::uint32_t dropped_notification_count() const;

  bool initialized() const { return initialized_; }
  bool host_ready() const;
  bool trainer_services_ready() const;
  bool heart_rate_services_ready() const;
  bool trainer_control_valid() const;
  bool trainer_control_pending() const;
  TrainerCapabilities trainer_capabilities() const;
  const BleConnectionManager& connection_manager() const { return connections_; }
  void set_wait_observer(std::function<void(std::uint64_t)> observer) {
    wait_observer_ = std::move(observer);
  }
#ifndef ESP_PLATFORM
  Status host_inject_notification(const BleNotification& notification);
#endif

 private:
  Status send_and_wait(FtmsOpcode opcode, const std::uint8_t* payload,
                       std::size_t length);

  std::unique_ptr<NimbleRuntime> runtime_;
  BleConnectionManager connections_;
  bool initialized_ = false;
  bool scan_active_ = false;
  std::function<void(std::uint64_t)> wait_observer_;
#ifdef ESP_PLATFORM
  std::uint64_t scan_deadline_ms_ = 0U;
#endif
};

}  // namespace trainer_loop
