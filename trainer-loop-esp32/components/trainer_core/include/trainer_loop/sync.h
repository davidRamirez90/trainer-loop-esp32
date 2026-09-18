#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trainer_loop {

struct RemoteWorkoutEvent {
  std::string event_id;
  std::string name;
  std::string local_date;
  std::string sport;
  bool has_download = true;
};

struct RemoteProfile {
  std::optional<int> ftp_watts;
  std::optional<double> weight_kg;
};

Result<std::vector<RemoteWorkoutEvent>> supported_today_events(
    const std::vector<RemoteWorkoutEvent>& events, const std::string& local_date);
ProfileSettings apply_remote_profile(ProfileSettings local, const RemoteProfile& remote);

enum class SyncState : std::uint8_t { kNever, kRunning, kSuccess, kEmpty, kOffline, kAuthBlocked, kError };

const char* sync_state_label(SyncState state);

struct SyncMetadata {
  SyncState state = SyncState::kNever;
  std::string local_date;
  std::string timezone;
  std::uint64_t last_attempt_ms = 0U;
  std::uint64_t last_success_ms = 0U;
  std::string error_class;
};

}  // namespace trainer_loop
