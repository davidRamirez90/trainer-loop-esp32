#pragma once

#include "trainer_loop/clock.h"
#include "trainer_loop/erg_policy.h"
#include "trainer_loop/journal.h"
#include "trainer_loop/models.h"
#include "trainer_loop/physics.h"
#include "trainer_loop/result.h"
#include "trainer_loop/workout.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trainer_loop {

enum class RideState : std::uint8_t {
  kBoot,
  kProvisioning,
  kReady,
  kScanning,
  kDevicesConnected,
  kPreview,
  kRunning,
  kPaused,
  kFinishing,
  kComplete,
};

enum class PauseReason : std::uint8_t {
  kUser,
  kTrainerDisconnected,
  kPowerStale,
  kCommandFailure,
  kStorageFailure,
};

class ITrainerController {
 public:
  virtual ~ITrainerController() = default;
  virtual Status acquire_control() = 0;
  virtual Status set_target_power(int watts) = 0;
  virtual Status start_or_resume() = 0;
  virtual Status pause() = 0;
  virtual Status stop() = 0;
};

struct CoordinatorSnapshot {
  RideState state = RideState::kBoot;
  PauseReason pause_reason = PauseReason::kUser;
  ClockSnapshot clocks;
  SensorSnapshot sensors;
  VirtualPoint virtual_point;
  int target_power_w = 0;
  bool resistance_unknown = false;
  bool simulation_enabled = true;
  bool complete = false;
  std::uint64_t now_ms = 0U;
  std::string fault;
};

class SessionCoordinator {
 public:
  SessionCoordinator(ITrainerController* trainer, SessionJournal* journal);

  Status enter_ready();
  Status set_trainer_capabilities(TrainerCapabilities capabilities);
  Status select_workout(Workout workout, ProfileSettings profile, bool simulation_enabled);
  Status start(std::uint64_t now_ms, StartTimeAnchor anchor, const SensorSnapshot& sensors,
               bool storage_reserved, std::string destination_athlete_id = {});
  Status tick(std::uint64_t now_ms, const SensorSnapshot& sensors);
  // Advances clocks, safety checks, telemetry, and recording while the
  // controller is synchronously waiting for an acknowledgement. This path
  // never issues another controller command.
  Status tick_control_suppressed(std::uint64_t now_ms, const SensorSnapshot& sensors);
  Status pause(PauseReason reason, std::uint64_t now_ms);
  Status resume(std::uint64_t now_ms, const SensorSnapshot& sensors);
  Status end_and_save(std::uint64_t now_ms, bool interrupted = false);
  Status discard(std::uint64_t now_ms);

  const CoordinatorSnapshot& snapshot() const { return snapshot_; }
  const Workout* selected_workout() const {
    return workout_.segments.empty() ? nullptr : &workout_;
  }
  const std::string& session_uuid() const { return session_uuid_; }
  const StartTimeAnchor& session_anchor() const { return anchor_; }
  std::uint32_t recorded_sample_count() const { return next_sample_sequence_; }
  const std::vector<TelemetrySample>& buffered_samples() const { return buffered_samples_; }
  const std::vector<SessionEvent>& events() const { return events_; }

 private:
  Status append_event(SessionEvent::Type type, std::uint64_t now_ms, const std::string& reason);
  Status flush_samples();
  Status record_sample(std::uint64_t now_ms, const SensorSnapshot& sensors,
                       int commanded_target_w);
  Status pause_without_controller(PauseReason reason, std::uint64_t now_ms);
  Status finish_deferred_safety_pause(std::uint64_t now_ms);
  bool trainer_inputs_fresh(const SensorSnapshot& sensors, std::uint64_t now_ms) const;
  Status finish(std::uint64_t now_ms, bool interrupted);
  Result<int> quantized_target(int watts) const;
  Result<int> restart_target(int interval_target) const;
  std::string make_session_uuid(std::uint64_t now_ms) const;

  ITrainerController* trainer_ = nullptr;
  SessionJournal* journal_ = nullptr;
  RideState state_ = RideState::kBoot;
  Workout workout_;
  ProfileSettings profile_;
  PhysicsParams physics_;
  std::optional<VirtualRideTracker> simulator_;
  std::optional<TrainerCapabilities> trainer_capabilities_;
  SessionClocks clocks_;
  StartTimeAnchor anchor_;
  std::string session_uuid_;
  std::string destination_athlete_id_;
  std::uint32_t next_sample_sequence_ = 0U;
  std::uint64_t next_sample_due_ms_ = 0U;
  std::uint32_t ramp_start_active_ms_ = 0U;
  int last_commanded_target_w_ = -1;
  bool stop_event_appended_ = false;
  bool finishing_interrupted_ = false;
  bool finish_pending_ = false;
  bool safety_pause_pending_ = false;
  std::uint64_t last_wait_now_ms_ = 0U;
  CoordinatorSnapshot snapshot_;
  std::vector<TelemetrySample> buffered_samples_;
  std::vector<SessionEvent> events_;
};

const char* ride_state_name(RideState state);
const char* pause_reason_name(PauseReason reason);

}  // namespace trainer_loop
