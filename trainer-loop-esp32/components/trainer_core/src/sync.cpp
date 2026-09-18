#include "trainer_loop/sync.h"

#include "trainer_loop/limits.h"
#include "trainer_loop/settings.h"

#include <cctype>
#include <cmath>

namespace trainer_loop {
namespace {

std::string lower(std::string value) {
  for (char& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return value;
}

bool cycling_sport(const std::string& sport) {
  const std::string normalized = lower(sport);
  return normalized.empty() || normalized == "cycling" || normalized == "bike" || normalized == "ride" ||
         normalized == "indoor_cycling" || normalized == "indoor cycling" || normalized == "virtualride" ||
         normalized == "virtual_ride" || normalized == "virtual ride";
}

bool safe_event_id(const std::string& value) {
  if (value.empty() || value.size() > 96U) return false;
  for (const char character : value) {
    if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '-' || character == '_')) return false;
  }
  return true;
}

}  // namespace

const char* sync_state_label(SyncState state) {
  switch (state) {
    case SyncState::kNever: return "Not synced";
    case SyncState::kRunning: return "Syncing";
    case SyncState::kSuccess: return "Synced";
    case SyncState::kEmpty: return "No workout today";
    case SyncState::kOffline: return "Offline";
    case SyncState::kAuthBlocked: return "Auth blocked";
    case SyncState::kError: return "Sync failed";
  }
  return "Sync failed";
}

Result<std::vector<RemoteWorkoutEvent>> supported_today_events(
    const std::vector<RemoteWorkoutEvent>& events, const std::string& local_date) {
  if (local_date.empty()) return Result<std::vector<RemoteWorkoutEvent>>::failure(ErrorCode::kInvalidArgument, "local date is empty");
  std::vector<RemoteWorkoutEvent> result;
  for (const auto& event : events) {
    if (event.local_date != local_date || !cycling_sport(event.sport) || !event.has_download) continue;
    if (!safe_event_id(event.event_id) || event.name.size() > kMaxWorkoutNameBytes) {
      return Result<std::vector<RemoteWorkoutEvent>>::failure(ErrorCode::kMalformedInput, "remote event is invalid");
    }
    result.push_back(event);
  }
  return Result<std::vector<RemoteWorkoutEvent>>::success(std::move(result));
}

ProfileSettings apply_remote_profile(ProfileSettings local, const RemoteProfile& remote) {
  if (!local.ftp_local_override && remote.ftp_watts.has_value() &&
      *remote.ftp_watts >= kFtpMinWatts && *remote.ftp_watts <= kFtpMaxWatts) {
    local.ftp_watts = *remote.ftp_watts;
  }
  if (!local.weight_local_override && remote.weight_kg.has_value() && std::isfinite(*remote.weight_kg) &&
      *remote.weight_kg >= kRiderMinKg && *remote.weight_kg <= kRiderMaxKg) {
    local.rider_kg = *remote.weight_kg;
  }
  return local;
}

}  // namespace trainer_loop
