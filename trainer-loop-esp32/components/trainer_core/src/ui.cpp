#include "trainer_loop/ui.h"

#include <algorithm>
#include <cmath>

namespace trainer_loop {

bool session_is_active(RideState state) {
  return state == RideState::kRunning || state == RideState::kPaused || state == RideState::kFinishing;
}

Status save_ui_profile(SettingsStore& store, ProfileSettings profile, RideState state,
                       const std::function<Status(const SettingsGeneration&)>& persist) {
  if (session_is_active(state)) return Status::failure(ErrorCode::kConflict, "Settings locked during a ride");
  profile.ftp_local_override = true;
  profile.weight_local_override = true;
  SettingsStore candidate = store;
  const Status updated = candidate.update_profile(profile, store.current().generation);
  if (!updated) return updated;
  const Status saved = persist(candidate.current());
  if (!saved) return saved;
  return store.restore(candidate.current());
}

Status adjust_ui_profile(ProfileSettings* profile, std::size_t field, int direction) {
  if (profile == nullptr || field > 4 || (direction != -1 && direction != 1))
    return Status::failure(ErrorCode::kBounds, "Invalid settings adjustment");
  auto candidate = *profile;
  switch (field) {
    case 0: candidate.ftp_watts += direction * 5; break;
    case 1: candidate.rider_kg = std::round((candidate.rider_kg + direction * 0.5) * 10) / 10; break;
    case 2: candidate.bike_kg = std::round((candidate.bike_kg + direction * 0.1) * 10) / 10; break;
    case 3: candidate.crr = std::round((candidate.crr + direction * 0.0005) * 10000) / 10000; break;
    case 4: candidate.cda = std::round((candidate.cda + direction * 0.01) * 100) / 100; break;
  }
  const Status valid = validate_profile(candidate);
  if (valid) *profile = candidate;
  return valid;
}

TodayWorkoutSelection next_today_workout(TodayWorkoutSelection current) {
  if (current.count == 0U) return TodayWorkoutSelection{};
  if (current.index >= current.count - 1U) current.index = 0U;
  else ++current.index;
  return current;
}

bool UiCommandQueue::push(UiCommand command) {
  if (size_ >= kCapacity) return false;
  commands_[tail_] = command;
  tail_ = (tail_ + 1U) % kCapacity;
  ++size_;
  return true;
}

std::optional<UiCommand> UiCommandQueue::pop() {
  if (size_ == 0U) return std::nullopt;
  const UiCommand command = commands_[head_];
  head_ = (head_ + 1U) % kCapacity;
  --size_;
  return command;
}

namespace {

std::string link_label(const char* name, LinkState state) {
  switch (state) {
    case LinkState::kConnected: return std::string(name) + " connected";
    case LinkState::kConnecting: return std::string(name) + " connecting";
    case LinkState::kReconnecting: return std::string(name) + " reconnecting";
    case LinkState::kFault: return std::string(name) + " fault";
    case LinkState::kDisconnected: return std::string(name) + " disconnected";
  }
  return std::string(name) + " unavailable";
}

}  // namespace

UiSnapshot make_ui_snapshot(const CoordinatorSnapshot& coordinator, const Workout* workout) {
  UiSnapshot snapshot;
  snapshot.trainer_status = link_label("Trainer", coordinator.sensors.trainer_link);
  snapshot.heart_rate_status = link_label("HR", coordinator.sensors.heart_rate_link);
  snapshot.wifi_status = "Wi-Fi status unavailable";
  snapshot.target_power_w = coordinator.target_power_w;
  snapshot.elapsed_active_ms = coordinator.clocks.active_ms;
  snapshot.elapsed_wall_ms = coordinator.clocks.wall_ms;
  snapshot.actual_power_w = coordinator.sensors.power_w.fresh(coordinator.now_ms, kPowerStaleAfterMs)
                                ? std::optional<int>(coordinator.sensors.power_w.value)
                                : std::nullopt;
  snapshot.heart_rate_bpm = coordinator.sensors.heart_rate_bpm.fresh(coordinator.now_ms, kHeartRateStaleAfterMs)
                                ? std::optional<int>(coordinator.sensors.heart_rate_bpm.value)
                                : std::nullopt;
  snapshot.cadence_rpm = coordinator.sensors.cadence_rpm.fresh(coordinator.now_ms, kPowerStaleAfterMs)
                             ? std::optional<double>(coordinator.sensors.cadence_rpm.value)
                             : std::nullopt;
  if (coordinator.virtual_point.speed_valid) {
    snapshot.speed_kph = coordinator.virtual_point.speed_mps * 3.6;
    snapshot.distance_km = coordinator.virtual_point.distance_m / 1000.0;
    snapshot.relative_ascent_m = coordinator.virtual_point.ascent_m;
    snapshot.grade_percent = coordinator.virtual_point.grade_percent;
  }
  snapshot.simulation_is_virtual = coordinator.simulation_enabled;
  if (workout != nullptr) {
    snapshot.workout_name = workout->name;
    snapshot.workout_duration_ms = workout->duration_ms();
    snapshot.interval_count = workout->segments.size();
    const std::uint32_t duration_ms = workout->duration_ms();
    const std::uint32_t plan_ms = std::min(coordinator.clocks.plan_ms, duration_ms);
    if (duration_ms > 0U) {
      const std::uint64_t scaled_progress = static_cast<std::uint64_t>(plan_ms) * 100U /
                                            static_cast<std::uint64_t>(duration_ms);
      snapshot.workout_progress_percent = static_cast<std::uint8_t>(std::min<std::uint64_t>(scaled_progress, 100U));
    }
    std::uint32_t cursor_ms = 0U;
    for (std::size_t index = 0U; index < workout->segments.size(); ++index) {
      const std::uint32_t segment_duration = workout->segments[index].duration_ms;
      const std::uint64_t segment_end = static_cast<std::uint64_t>(cursor_ms) + segment_duration;
      if (plan_ms < segment_end || index + 1U == workout->segments.size()) {
        snapshot.interval_index = index;
        snapshot.interval_elapsed_ms = plan_ms > cursor_ms ? plan_ms - cursor_ms : 0U;
        snapshot.interval_remaining_ms = segment_duration > snapshot.interval_elapsed_ms
                                             ? segment_duration - snapshot.interval_elapsed_ms
                                             : 0U;
        break;
      }
      cursor_ms = static_cast<std::uint32_t>(segment_end);
    }
    if (snapshot.interval_count > 0U) {
      snapshot.interval_status = std::to_string(snapshot.interval_index + 1U) + "/" +
                                 std::to_string(snapshot.interval_count);
    }
  }
  switch (coordinator.state) {
    case RideState::kRunning:
      snapshot.screen = UiScreen::kActive;
      snapshot.pause_available = true;
      snapshot.end_save_available = true;
      break;
    case RideState::kPaused:
      snapshot.screen = UiScreen::kPaused;
      // A trainer-loss pause intentionally keeps resistance_unknown set until
      // the user completes an explicit, acknowledged resume handshake.  The
      // button must become available again once fresh power is back; hiding it
      // behind resistance_unknown would strand every reconnect in Paused.
      snapshot.resume_available = coordinator.sensors.trainer_link == LinkState::kConnected &&
                                  coordinator.sensors.telemetry_subscription &&
                                  coordinator.sensors.power_w.fresh(coordinator.now_ms, kPowerStaleAfterMs);
      snapshot.end_save_available = true;
      break;
    case RideState::kPreview:
      snapshot.screen = UiScreen::kPreview;
      snapshot.start_available = !coordinator.resistance_unknown;
      break;
    case RideState::kFinishing:
      snapshot.screen = UiScreen::kError;
      snapshot.fault = "Save incomplete. Retry keeps the same session.";
      break;
    case RideState::kComplete:
      snapshot.screen = UiScreen::kCompletion;
      break;
    default:
      snapshot.screen = UiScreen::kHome;
      break;
  }
  snapshot.devices_available = snapshot.screen == UiScreen::kHome || snapshot.screen == UiScreen::kPreview;
  snapshot.settings_available = !session_is_active(coordinator.state);
  snapshot.retry_available = !session_is_active(coordinator.state) || coordinator.state == RideState::kFinishing;
  if (coordinator.resistance_unknown || coordinator.pause_reason == PauseReason::kTrainerDisconnected) {
    snapshot.safety_message = "STOP PEDALING — resistance unknown";
  } else if (coordinator.pause_reason == PauseReason::kPowerStale) {
    snapshot.safety_message = "Power stale — workout paused";
  }
  if (!coordinator.fault.empty()) snapshot.fault = coordinator.fault;
  return snapshot;
}

const char* ui_screen_name(UiScreen screen) {
  switch (screen) {
    case UiScreen::kSetup: return "Setup";
    case UiScreen::kHome: return "Home";
    case UiScreen::kPairing: return "Pairing";
    case UiScreen::kPreview: return "Workout Preview";
    case UiScreen::kActive: return "Active";
    case UiScreen::kPaused: return "Paused";
    case UiScreen::kStopConfirmation: return "Stop Confirmation";
    case UiScreen::kCompletion: return "Completion";
    case UiScreen::kUploadStatus: return "Upload Status";
    case UiScreen::kHistory: return "History";
    case UiScreen::kHistoryDetail: return "History Detail";
    case UiScreen::kSettings: return "Settings";
    case UiScreen::kError: return "Error";
  }
  return "Unknown";
}

}  // namespace trainer_loop
