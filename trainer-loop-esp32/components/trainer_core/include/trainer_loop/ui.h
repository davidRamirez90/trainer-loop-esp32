#pragma once

#include "trainer_loop/ble_manager.h"
#include "trainer_loop/models.h"
#include "trainer_loop/result.h"
#include "trainer_loop/state_machine.h"

#include "trainer_loop/settings.h"
#include <functional>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trainer_loop {

enum class UiScreen : std::uint8_t {
  kSetup,
  kHome,
  kPairing,
  kPreview,
  kActive,
  kPaused,
  kStopConfirmation,
  kCompletion,
  kUploadStatus,
  kHistory,
  kHistoryDetail,
  kSettings,
  kError,
};

enum class UiCommandType : std::uint8_t {
  kBack,
  kStart,
  kPause,
  kResume,
  kEndAndSave,
  kOpenDevices,
  kOpenHistory,
  kOpenHistoryEntry,
  kOpenSettings,
  kRetry,
  kSelectDevice,
  kProfileNext,
  kProfileDecrease,
  kProfileIncrease,
  kSaveProfile,
};

struct UiCommand {
  UiCommandType type = UiCommandType::kBack;
  std::size_t index = 0U;
};

class UiCommandQueue {
 public:
  static constexpr std::size_t kCapacity = 16U;
  bool push(UiCommand command);
  std::optional<UiCommand> pop();
  std::size_t size() const { return size_; }

 private:
  UiCommand commands_[kCapacity]{};
  std::size_t head_ = 0U;
  std::size_t tail_ = 0U;
  std::size_t size_ = 0U;
};

struct UiHistoryEntry {
  SessionSummary summary;
  std::string upload_status;
};

struct UiSnapshot {
  UiScreen screen = UiScreen::kHome;
  std::string trainer_status;
  std::string heart_rate_status;
  std::string wifi_status;
  std::string workout_name;
  std::string interval_status;
  std::string fault;
  std::string safety_message;
  std::string completion_upload_status;
  std::uint32_t elapsed_active_ms = 0U;
  std::uint32_t elapsed_wall_ms = 0U;
  std::uint32_t workout_duration_ms = 0U;
  int target_power_w = 0;
  std::optional<int> actual_power_w;
  std::optional<int> heart_rate_bpm;
  std::optional<double> cadence_rpm;
  std::optional<double> speed_kph;
  std::optional<double> grade_percent;
  std::optional<double> distance_km;
  std::optional<double> relative_ascent_m;
  std::vector<DeviceIdentity> device_candidates;
  std::vector<UiHistoryEntry> history_entries;
  std::optional<SessionSummary> completion_summary;
  std::optional<UiHistoryEntry> history_detail;
  ProfileSettings profile;
  std::size_t profile_field = 0U;
  bool settings_available = false;
  bool retry_available = false;
  bool pause_available = false;
  bool resume_available = false;
  bool end_save_available = false;
  bool start_available = false;
  bool devices_available = false;
  bool history_available = false;
  bool pairing_scan_active = false;
  bool busy = false;
  bool simulation_is_virtual = true;
};

bool session_is_active(RideState state);
Status save_ui_profile(SettingsStore& store, ProfileSettings profile, RideState state,
                       const std::function<Status(const SettingsGeneration&)>& persist);
Status adjust_ui_profile(ProfileSettings* profile, std::size_t field, int direction);
UiSnapshot make_ui_snapshot(const CoordinatorSnapshot& coordinator, const Workout* workout = nullptr);
const char* ui_screen_name(UiScreen screen);

}  // namespace trainer_loop
