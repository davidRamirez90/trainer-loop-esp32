#include "trainer_loop/state_machine.h"

#include "trainer_loop/hash.h"
#include "trainer_loop/limits.h"
#include "trainer_loop/settings.h"
#include "trainer_loop/time_service.h"

#include <algorithm>
#include <cmath>

namespace trainer_loop {
namespace {

constexpr std::size_t kMaxSessionEvents = 256U;
constexpr std::uint64_t kSamplePeriodMs = 1000U;

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
  return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

double midpoint(const TargetRange& range) { return (range.low + range.high) / 2.0; }

std::uint16_t cadence_raw(const SensorSnapshot& sensors, std::uint64_t now_ms) {
  if (!sensors.cadence_rpm.fresh(now_ms, kPowerStaleAfterMs)) return 0U;
  const double raw = std::round(std::max(0.0, sensors.cadence_rpm.value) * 2.0);
  return raw >= 65535.0 ? 65535U : static_cast<std::uint16_t>(raw);
}

}  // namespace

SessionCoordinator::SessionCoordinator(ITrainerController* trainer, SessionJournal* journal)
    : trainer_(trainer), journal_(journal) {
  snapshot_.state = state_;
}

Status SessionCoordinator::enter_ready() {
  if (state_ != RideState::kBoot && state_ != RideState::kProvisioning && state_ != RideState::kComplete) {
    return Status::failure(ErrorCode::kConflict, "cannot enter Ready from current state");
  }
  if (state_ == RideState::kComplete) {
    // A coordinator can own multiple rides over its lifetime. The completed
    // clock must not make the next Preview->Running transition fail with a
    // stale "clock already started" error.
    clocks_ = SessionClocks{};
    session_uuid_.clear();
    events_.clear();
    buffered_samples_.clear();
  }
  workout_ = Workout{};
  simulator_.reset();
  snapshot_ = CoordinatorSnapshot{};
  state_ = RideState::kReady;
  snapshot_.state = state_;
  return Status::ok();
}

Status SessionCoordinator::set_trainer_capabilities(TrainerCapabilities capabilities) {
  if (state_ == RideState::kRunning || state_ == RideState::kPaused || state_ == RideState::kFinishing) {
    return Status::failure(ErrorCode::kConflict, "trainer capabilities cannot change during a ride");
  }
  if (!capabilities.ftms_present || !capabilities.control_point_present || !capabilities.indoor_bike_data_present ||
      !capabilities.feature_present ||
      !capabilities.power_range_present || capabilities.power_min_watts < 0 ||
      capabilities.power_max_watts < capabilities.power_min_watts || capabilities.power_max_watts > 2000 ||
      capabilities.power_increment_watts <= 0) {
    return Status::failure(ErrorCode::kUnsupported, "trainer capabilities are incomplete");
  }
  trainer_capabilities_ = capabilities;
  return Status::ok();
}

Status SessionCoordinator::select_workout(Workout workout, ProfileSettings profile, bool simulation_enabled) {
  const Status profile_status = validate_profile(profile);
  if (!profile_status) return profile_status;
  const Status workout_status = validate_workout(workout, profile.ftp_watts);
  if (!workout_status) return workout_status;
  if (state_ != RideState::kReady && state_ != RideState::kDevicesConnected && state_ != RideState::kPreview) {
    return Status::failure(ErrorCode::kConflict, "workout can only be selected while idle");
  }
  workout_ = std::move(workout);
  profile_ = std::move(profile);
  snapshot_.pause_reason = PauseReason::kUser;
  snapshot_.resistance_unknown = false;
  snapshot_.fault.clear();
  snapshot_.complete = false;
  physics_ = PhysicsParams{profile_.rider_kg, profile_.bike_kg, profile_.crr, profile_.cda};
  if (simulation_enabled) {
    const auto route = generate_route(workout_, profile_.ftp_watts, physics_);
    if (!route) return Status::failure(route.error->code, route.error->message);
    simulator_.emplace(std::move(*route.value), physics_);
  } else {
    simulator_.reset();
  }
  snapshot_.simulation_enabled = simulation_enabled;
  state_ = RideState::kPreview;
  snapshot_.state = state_;
  snapshot_.target_power_w = static_cast<int>(std::lround(midpoint(resolved_target_range_at(workout_, 0U, profile_.ftp_watts))));
  return Status::ok();
}

Status SessionCoordinator::start(std::uint64_t now_ms, StartTimeAnchor anchor,
                                 const SensorSnapshot& sensors, bool storage_reserved) {
  if (state_ != RideState::kPreview) return Status::failure(ErrorCode::kConflict, "workout is not in preview");
  if (!anchor.trusted || !plausible_utc_ms(anchor.utc_ms)) {
    return Status::failure(ErrorCode::kTimeUntrusted, "trusted plausible time is required to start");
  }
  if (!storage_reserved) return Status::failure(ErrorCode::kStorage, "recording reservation is unavailable");
  if (journal_ == nullptr) return Status::failure(ErrorCode::kStorage, "durable session journal is unavailable");
  if (!trainer_capabilities_.has_value()) {
    return Status::failure(ErrorCode::kUnsupported, "trainer power capabilities are unavailable");
  }
  if (trainer_ == nullptr || sensors.trainer_link != LinkState::kConnected ||
      !sensors.telemetry_subscription ||
      !sensors.power_w.fresh(now_ms, kPowerStaleAfterMs)) {
    return Status::failure(ErrorCode::kTransport, "fresh trainer power and telemetry are required");
  }
  const Status range_status = validate_workout_power_range(workout_, profile_.ftp_watts, *trainer_capabilities_);
  if (!range_status) return range_status;
  const auto target = resolved_target_range_at(workout_, 0U, profile_.ftp_watts);
  const int interval_target = static_cast<int>(std::lround(midpoint(target)));
  const auto restart = restart_target(interval_target);
  if (!restart) return Status::failure(restart.error->code, restart.error->message);
  const int safe_restart_target = *restart.value;
  Status status = trainer_->acquire_control();
  if (!status) return status;
  status = trainer_->set_target_power(safe_restart_target);
  if (!status) {
    (void)trainer_->stop();
    return status;
  }
  status = trainer_->start_or_resume();
  if (!status) {
    (void)trainer_->stop();
    return status;
  }

  session_uuid_ = make_session_uuid(now_ms);
  buffered_samples_.clear();
  events_.clear();
  snapshot_.virtual_point = VirtualPoint{};
  JournalManifest manifest;
  manifest.uuid = session_uuid_;
  manifest.workout_id = workout_.id;
  manifest.workout_name = workout_.name;
  manifest.start_utc_ms = anchor.utc_ms;
  manifest.time_trusted = anchor.trusted;
  manifest.time_quality = anchor.quality;
  status = journal_->start(manifest);
  if (!status) {
    (void)trainer_->stop();
    return status;
  }
  status = clocks_.start(now_ms, anchor);
  if (!status) {
    (void)trainer_->stop();
    return status;
  }
  anchor_ = anchor;
  const Status start_event = append_event(SessionEvent::Type::kStart, now_ms, "start");
  if (!start_event) {
    (void)trainer_->stop();
    (void)clocks_.finish(now_ms);
    return start_event;
  }
  state_ = RideState::kRunning;
  next_sample_sequence_ = 0U;
  next_sample_due_ms_ = now_ms;
  ramp_start_active_ms_ = 0U;
  last_commanded_target_w_ = safe_restart_target;
  snapshot_.state = state_;
  snapshot_.now_ms = now_ms;
  snapshot_.clocks = clocks_.snapshot(now_ms);
  snapshot_.resistance_unknown = false;
  snapshot_.pause_reason = PauseReason::kUser;
  snapshot_.fault.clear();
  snapshot_.sensors = sensors;
  snapshot_.target_power_w = safe_restart_target;
  stop_event_appended_ = false;
  finishing_interrupted_ = false;
  return Status::ok();
}

Status SessionCoordinator::tick(std::uint64_t now_ms, const SensorSnapshot& sensors) {
  snapshot_.now_ms = now_ms;
  snapshot_.sensors = sensors;
  snapshot_.clocks = clocks_.snapshot(now_ms);
  if (state_ != RideState::kRunning) return Status::ok();
  if (sensors.trainer_link != LinkState::kConnected) {
    return pause(PauseReason::kTrainerDisconnected, now_ms);
  }
  if (!sensors.telemetry_subscription || !sensors.power_w.fresh(now_ms, kPowerStaleAfterMs)) {
    return pause(PauseReason::kPowerStale, now_ms);
  }
  const ClockSnapshot clocks = clocks_.snapshot(now_ms);
  const TargetRange target_range = resolved_target_range_at(workout_, clocks.plan_ms, profile_.ftp_watts);
  const auto interval_target_result = quantized_target(static_cast<int>(std::lround(midpoint(target_range))));
  if (!interval_target_result) return pause(PauseReason::kCommandFailure, now_ms);
  const int interval_target = *interval_target_result.value;
  int commanded_target = interval_target;
  const std::uint32_t ramp_end_active_ms = ramp_start_active_ms_ > UINT32_MAX - 3000U
                                               ? UINT32_MAX
                                               : ramp_start_active_ms_ + 3000U;
  if (clocks.active_ms < ramp_end_active_ms) {
    const double ratio = static_cast<double>(clocks.active_ms - ramp_start_active_ms_) / 3000.0;
    commanded_target = static_cast<int>(std::lround(static_cast<double>(last_commanded_target_w_) +
                                                     (interval_target - last_commanded_target_w_) * std::clamp(ratio, 0.0, 1.0)));
  }
  if (commanded_target != last_commanded_target_w_ || now_ms >= next_sample_due_ms_) {
    const Status command = trainer_->set_target_power(commanded_target);
    if (!command) return pause(PauseReason::kCommandFailure, now_ms);
    last_commanded_target_w_ = commanded_target;
    snapshot_.target_power_w = commanded_target;
  }
  if (now_ms < next_sample_due_ms_) return Status::ok();
  next_sample_due_ms_ = saturating_add(now_ms, kSamplePeriodMs);
  VirtualPoint point;
  if (simulator_.has_value()) point = simulator_->tick(clocks.plan_ms, sensors.power_w.value, true);
  const bool hr_fresh = sensors.heart_rate_bpm.fresh(now_ms, kHeartRateStaleAfterMs);
  TelemetrySample sample;
  sample.sequence = next_sample_sequence_++;
  sample.wall_ms = clocks.wall_ms;
  sample.active_ms = clocks.active_ms;
  sample.plan_ms = clocks.plan_ms;
  sample.power_w = static_cast<std::int16_t>(std::clamp(sensors.power_w.value, -32768, 32767));
  sample.target_w = static_cast<std::uint16_t>(std::clamp(commanded_target, 0, 65535));
  sample.cadence_half_rpm = cadence_raw(sensors, now_ms);
  sample.heart_rate_bpm = hr_fresh ? static_cast<std::uint16_t>(std::clamp(sensors.heart_rate_bpm.value, 0, 65535)) : 0U;
  sample.validity = kSamplePowerValid | kSampleTrainerLink;
  if (sensors.cadence_rpm.fresh(now_ms, kPowerStaleAfterMs)) sample.validity |= kSampleCadenceValid;
  if (hr_fresh) sample.validity |= kSampleHeartRateValid | kSampleHeartRateLink;
  if (simulator_.has_value()) {
    sample.speed_mm_s = point.speed_valid ? static_cast<std::uint32_t>(std::max(0.0, std::round(point.speed_mps * 1000.0))) : 0U;
    sample.distance_cm = static_cast<std::uint32_t>(std::max(0.0, std::round(point.distance_m * 100.0)));
    sample.relative_altitude_cm = static_cast<std::int32_t>(std::clamp(std::round(point.altitude_m * 100.0), -2147483647.0, 2147483647.0));
    sample.grade_hundredths_percent = static_cast<std::int16_t>(std::clamp(std::round(point.grade_percent * 100.0), -32768.0, 32767.0));
    sample.validity |= kSampleSimulationEnabled | kSampleGradeValid;
    if (point.speed_valid) sample.validity |= kSampleSpeedValid | kSampleDistanceValid | kSampleAltitudeValid;
  }
  buffered_samples_.push_back(sample);
  snapshot_.virtual_point = point;
  snapshot_.clocks = clocks;
  if (buffered_samples_.size() >= 5U) {
    const Status flushed = flush_samples();
    if (!flushed) return pause(PauseReason::kStorageFailure, now_ms);
  }
  if (clocks.plan_ms >= workout_.duration_ms()) return finish(now_ms, false);
  return Status::ok();
}

Status SessionCoordinator::pause(PauseReason reason, std::uint64_t now_ms) {
  if (state_ != RideState::kRunning) return Status::failure(ErrorCode::kConflict, "ride is not running");
  const Status clock_status = clocks_.pause(now_ms);
  if (!clock_status) return clock_status;
  Status controller_status = trainer_ == nullptr ? Status::failure(ErrorCode::kTransport, "trainer controller is missing") : trainer_->pause();
  state_ = RideState::kPaused;
  snapshot_.state = state_;
  snapshot_.pause_reason = reason;
  // A lost link is always resistance-unknown: a best-effort pause call cannot
  // be considered acknowledged after transport loss.
  snapshot_.resistance_unknown = reason == PauseReason::kTrainerDisconnected || !controller_status;
  snapshot_.fault = controller_status ? "" : controller_status.error->message;
  const Status event_status = append_event(
      reason == PauseReason::kTrainerDisconnected ? SessionEvent::Type::kTrainerDisconnected : SessionEvent::Type::kPause,
      now_ms, pause_reason_name(reason));
  if (!event_status) return event_status;
  const Status flushed = flush_samples();
  if (!flushed) return flushed;
  snapshot_.now_ms = now_ms;
  snapshot_.clocks = clocks_.snapshot(now_ms);
  return Status::ok();
}

Status SessionCoordinator::resume(std::uint64_t now_ms, const SensorSnapshot& sensors) {
  if (state_ != RideState::kPaused) return Status::failure(ErrorCode::kConflict, "ride is not paused");
  if (trainer_ == nullptr || sensors.trainer_link != LinkState::kConnected ||
      !sensors.telemetry_subscription ||
      !sensors.power_w.fresh(now_ms, kPowerStaleAfterMs) || !trainer_capabilities_.has_value()) {
    return Status::failure(ErrorCode::kTransport, "resume requires fresh trainer power and link");
  }
  Status status = trainer_->acquire_control();
  if (!status) return status;
  const ClockSnapshot clocks = clocks_.snapshot(now_ms);
  const auto target_result = quantized_target(static_cast<int>(std::lround(midpoint(
      resolved_target_range_at(workout_, clocks.plan_ms, profile_.ftp_watts)))));
  if (!target_result) return Status::failure(target_result.error->code, target_result.error->message);
  const auto restart = restart_target(*target_result.value);
  if (!restart) return Status::failure(restart.error->code, restart.error->message);
  const int restart_target_w = *restart.value;
  status = trainer_->set_target_power(restart_target_w);
  if (!status) {
    (void)trainer_->stop();
    return status;
  }
  status = trainer_->start_or_resume();
  if (!status) {
    (void)trainer_->stop();
    return status;
  }
  status = clocks_.resume(now_ms);
  if (!status) {
    (void)trainer_->stop();
    return status;
  }
  const Status resume_event = append_event(SessionEvent::Type::kResume, now_ms, "resume");
  if (!resume_event) {
    (void)trainer_->stop();
    (void)clocks_.pause(now_ms);
    snapshot_.state = RideState::kPaused;
    snapshot_.pause_reason = PauseReason::kStorageFailure;
    snapshot_.resistance_unknown = true;
    snapshot_.fault = resume_event.error->message;
    snapshot_.now_ms = now_ms;
    snapshot_.clocks = clocks_.snapshot(now_ms);
    return resume_event;
  }
  state_ = RideState::kRunning;
  snapshot_.state = state_;
  snapshot_.resistance_unknown = false;
  snapshot_.pause_reason = PauseReason::kUser;
  snapshot_.fault.clear();
  snapshot_.sensors = sensors;
  ramp_start_active_ms_ = clocks.active_ms;
  last_commanded_target_w_ = restart_target_w;
  next_sample_due_ms_ = now_ms;
  snapshot_.now_ms = now_ms;
  snapshot_.clocks = clocks_.snapshot(now_ms);
  return Status::ok();
}

Result<int> SessionCoordinator::quantized_target(int watts) const {
  if (!trainer_capabilities_.has_value()) {
    return Result<int>::failure(ErrorCode::kUnsupported, "trainer power capabilities are unavailable");
  }
  return ErgPowerNegotiator(*trainer_capabilities_).validate_target(watts);
}

Result<int> SessionCoordinator::restart_target(int interval_target) const {
  if (interval_target < 0) {
    return Result<int>::failure(ErrorCode::kBounds, "interval target is negative");
  }
  if (!trainer_capabilities_.has_value()) {
    return Result<int>::failure(ErrorCode::kUnsupported, "trainer power capabilities are unavailable");
  }
  const int requested = std::max(trainer_capabilities_->power_min_watts,
                                 std::min(interval_target, std::max(1, std::min(50, interval_target))));
  if (requested > interval_target) {
    return Result<int>::failure(ErrorCode::kBounds, "trainer minimum target exceeds interval target");
  }
  // Choose the lower supported step for the restart ramp. The normal target
  // path uses nearest-step quantization, but a restart must never jump above
  // the interval merely because the trainer increment is coarse.
  const int delta = requested - trainer_capabilities_->power_min_watts;
  const int floored = trainer_capabilities_->power_min_watts +
                      (delta / trainer_capabilities_->power_increment_watts) *
                          trainer_capabilities_->power_increment_watts;
  return quantized_target(floored);
}

Status SessionCoordinator::end_and_save(std::uint64_t now_ms, bool interrupted) {
  if (state_ == RideState::kFinishing) return finish(now_ms, finishing_interrupted_);
  if (state_ != RideState::kRunning && state_ != RideState::kPaused) {
    return Status::failure(ErrorCode::kConflict, "there is no active ride");
  }
  if (state_ == RideState::kRunning) {
    const Status paused = pause(PauseReason::kUser, now_ms);
    if (!paused) return paused;
  }
  return finish(now_ms, interrupted);
}

Status SessionCoordinator::finish(std::uint64_t now_ms, bool interrupted) {
  if (state_ == RideState::kComplete) return Status::ok();
  if (state_ != RideState::kFinishing) {
    state_ = RideState::kFinishing;
    finishing_interrupted_ = interrupted;
    snapshot_.state = state_;
    if (trainer_ != nullptr) {
      const Status stopped = trainer_->stop();
      if (!stopped) snapshot_.resistance_unknown = true;
    }
  }
  if (!stop_event_appended_) {
    const Status event = append_event(SessionEvent::Type::kStop, now_ms,
                                      finishing_interrupted_ ? "interrupted" : "stop");
    if (!event) return event;
    stop_event_appended_ = true;
  }
  const Status flushed = flush_samples();
  if (!flushed) return flushed;
  if (clocks_.running()) {
    const Status clock_finished = clocks_.finish(now_ms);
    if (!clock_finished) return clock_finished;
  }
  const ClockSnapshot clocks = clocks_.snapshot(now_ms);
  const bool naturally_complete = clocks.plan_ms >= workout_.duration_ms();
  finishing_interrupted_ = finishing_interrupted_ || !naturally_complete;
  if (journal_ != nullptr) {
    JournalManifest manifest;
    manifest.uuid = session_uuid_;
    manifest.workout_id = workout_.id;
    manifest.workout_name = workout_.name;
    manifest.start_utc_ms = anchor_.utc_ms;
    manifest.time_trusted = anchor_.trusted;
    manifest.time_quality = anchor_.quality;
    manifest.wall_ms = clocks.wall_ms;
    manifest.active_ms = clocks.active_ms;
    manifest.plan_ms = clocks.plan_ms;
    manifest.complete = !finishing_interrupted_ && naturally_complete;
    manifest.interrupted = finishing_interrupted_;
    const Status finalized = journal_->finalize(manifest);
    if (!finalized) return finalized;
  }
  snapshot_.complete = !finishing_interrupted_ && naturally_complete;
  snapshot_.clocks = clocks;
  state_ = RideState::kComplete;
  snapshot_.state = state_;
  return Status::ok();
}

Status SessionCoordinator::append_event(SessionEvent::Type type, std::uint64_t now_ms,
                                        const std::string& reason) {
  if (events_.size() >= kMaxSessionEvents) {
    return Status::failure(ErrorCode::kBounds, "session event log is full");
  }
  const ClockSnapshot clocks = clocks_.snapshot(now_ms);
  SessionEvent event{type, clocks.wall_ms, clocks.active_ms, clocks.plan_ms, reason};
  if (journal_ != nullptr) {
    const Status status = journal_->append_event(event);
    if (!status) return status;
  }
  events_.push_back(std::move(event));
  return Status::ok();
}

Status SessionCoordinator::flush_samples() {
  if (buffered_samples_.empty()) return Status::ok();
  if (journal_ != nullptr) {
    const Status status = journal_->append_samples(buffered_samples_);
    if (!status) return status;
  }
  buffered_samples_.clear();
  return Status::ok();
}

std::string SessionCoordinator::make_session_uuid(std::uint64_t now_ms) const {
  const std::uint32_t first = fnv1a32(workout_.id + std::to_string(now_ms));
  const std::uint32_t second = fnv1a32(workout_.name + std::to_string(now_ms ^ first));
  return hex_u32(first) + "-" + hex_u32(second);
}

const char* ride_state_name(RideState state) {
  switch (state) {
    case RideState::kBoot: return "Boot";
    case RideState::kProvisioning: return "Provisioning";
    case RideState::kReady: return "Ready";
    case RideState::kScanning: return "Scanning";
    case RideState::kDevicesConnected: return "Devices Connected";
    case RideState::kPreview: return "Workout Preview";
    case RideState::kRunning: return "Running";
    case RideState::kPaused: return "Paused";
    case RideState::kFinishing: return "Finishing";
    case RideState::kComplete: return "Complete";
  }
  return "Unknown";
}

const char* pause_reason_name(PauseReason reason) {
  switch (reason) {
    case PauseReason::kUser: return "user";
    case PauseReason::kTrainerDisconnected: return "trainer_disconnected";
    case PauseReason::kPowerStale: return "power_stale";
    case PauseReason::kCommandFailure: return "command_failure";
    case PauseReason::kStorageFailure: return "storage_failure";
  }
  return "unknown";
}

}  // namespace trainer_loop
