#include "intervals_sync.h"

#ifdef ESP_PLATFORM

#include "cJSON.h"
#include "trainer_loop/workout.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace trainer_loop {
namespace {

constexpr std::size_t kMaxTodayEvents = 64U;

std::string object_string(const cJSON* object, const char* key) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsString(value) && value->valuestring != nullptr) return value->valuestring;
  if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
      std::trunc(value->valuedouble) == value->valuedouble &&
      value->valuedouble >= static_cast<double>(std::numeric_limits<long long>::min()) &&
      value->valuedouble <= static_cast<double>(std::numeric_limits<long long>::max())) {
    return std::to_string(static_cast<long long>(value->valuedouble));
  }
  return {};
}

std::optional<double> object_number(const cJSON* object, const char* key) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble)) return std::nullopt;
  return value->valuedouble;
}

Result<cJSON*> parse_json(const HttpResponse& response) {
  if (response.status != 200) return Result<cJSON*>::failure(ErrorCode::kTransport, "Intervals response was not successful");
  cJSON* root = cJSON_ParseWithLength(response.body.c_str(), response.body.size());
  if (root == nullptr) return Result<cJSON*>::failure(ErrorCode::kMalformedInput, "Intervals response JSON is invalid");
  return Result<cJSON*>::success(root);
}

}  // namespace

Result<SyncMetadata> IntervalsSync::sync_today(const TimeService& time) {
  if (http_ == nullptr || cache_ == nullptr || settings_ == nullptr) return Result<SyncMetadata>::failure(ErrorCode::kInvalidArgument, "Intervals sync dependencies are missing");
  SyncMetadata metadata;
  metadata.timezone = settings_->current().profile.timezone;
  metadata.local_date = local_date_for_utc(time.snapshot().utc_ms, settings_->current().profile.timezone_rule);
  if (!time.trusted() || metadata.local_date.empty()) return Result<SyncMetadata>::failure(ErrorCode::kTimeUntrusted, "trusted time is required for today sync");
  if (!settings_->has_committed_settings()) return Result<SyncMetadata>::failure(ErrorCode::kAuthentication, "Intervals credentials are not configured");
  metadata.last_attempt_ms = time.snapshot().utc_ms < 0 ? 0U : static_cast<std::uint64_t>(time.snapshot().utc_ms);
  const Secrets& secrets = settings_->current().secrets;
  const auto profile_response = http_->execute(build_athlete_request(secrets.athlete_id, secrets.api_key));
  if (!profile_response) return Result<SyncMetadata>::failure(profile_response.error->code, profile_response.error->message);
  if (profile_response.value->status == 401 || profile_response.value->status == 403) {
    metadata.state = SyncState::kAuthBlocked;
    metadata.error_class = "authentication";
    return Result<SyncMetadata>::success(metadata);
  }
  const auto profile_json = parse_json(*profile_response.value);
  if (!profile_json) return Result<SyncMetadata>::failure(profile_json.error->code, profile_json.error->message);
  cJSON* root = *profile_json.value;
  RemoteProfile remote;
  if (const auto ftp = object_number(root, "ftp"); ftp && std::trunc(*ftp) == *ftp &&
      *ftp >= static_cast<double>(std::numeric_limits<int>::min()) &&
      *ftp <= static_cast<double>(std::numeric_limits<int>::max())) {
    remote.ftp_watts = static_cast<int>(*ftp);
  }
  if (const auto weight = object_number(root, "icu_weight")) remote.weight_kg = *weight;
  ProfileSettings updated = apply_remote_profile(settings_->current().profile, remote);
  if (updated.ftp_watts != settings_->current().profile.ftp_watts || updated.rider_kg != settings_->current().profile.rider_kg) {
    (void)settings_->update_profile(updated, settings_->current().generation);
  }
  cJSON_Delete(root);

  const auto events_response = http_->execute(build_today_events_request(secrets.athlete_id, secrets.api_key, metadata.local_date));
  if (!events_response) return Result<SyncMetadata>::failure(events_response.error->code, events_response.error->message);
  if (events_response.value->status == 401 || events_response.value->status == 403) {
    metadata.state = SyncState::kAuthBlocked;
    metadata.error_class = "authentication";
    return Result<SyncMetadata>::success(metadata);
  }
  const auto events_json = parse_json(*events_response.value);
  if (!events_json) return Result<SyncMetadata>::failure(events_json.error->code, events_json.error->message);
  const cJSON* array = cJSON_IsArray(*events_json.value) ? *events_json.value : cJSON_GetObjectItemCaseSensitive(*events_json.value, "events");
  if (!cJSON_IsArray(array)) {
    cJSON_Delete(*events_json.value);
    return Result<SyncMetadata>::failure(ErrorCode::kMalformedInput, "Intervals events response is not an array");
  }
  std::vector<RemoteWorkoutEvent> remote_events;
  cJSON* event = nullptr;
  cJSON_ArrayForEach(event, array) {
    if (remote_events.size() >= kMaxTodayEvents) {
      cJSON_Delete(*events_json.value);
      return Result<SyncMetadata>::failure(ErrorCode::kBounds, "Intervals returned too many workout events");
    }
    RemoteWorkoutEvent descriptor;
    descriptor.event_id = object_string(event, "id");
    descriptor.name = object_string(event, "name");
    descriptor.local_date = object_string(event, "start_date_local");
    if (descriptor.local_date.size() >= 10U) descriptor.local_date.resize(10U);
    if (descriptor.local_date.empty()) descriptor.local_date = object_string(event, "date");
    descriptor.sport = object_string(event, "sport");
    if (descriptor.sport.empty()) descriptor.sport = object_string(event, "type");
    if (!descriptor.event_id.empty()) remote_events.push_back(std::move(descriptor));
  }
  cJSON_Delete(*events_json.value);
  const auto supported = supported_today_events(remote_events, metadata.local_date);
  if (!supported) return Result<SyncMetadata>::failure(supported.error->code, supported.error->message);
  if (supported.value->empty()) {
    metadata.state = SyncState::kEmpty;
    return Result<SyncMetadata>::success(metadata);
  }

  std::vector<CachedWorkout> pending;
  pending.reserve(supported.value->size());
  for (const auto& descriptor : *supported.value) {
    const auto zwo_response = http_->execute(build_zwo_request(secrets.athlete_id, secrets.api_key, descriptor.event_id));
    if (!zwo_response) return Result<SyncMetadata>::failure(zwo_response.error->code, zwo_response.error->message);
    if (zwo_response.value->status == 401 || zwo_response.value->status == 403) {
      metadata.state = SyncState::kAuthBlocked;
      metadata.error_class = "authentication";
      return Result<SyncMetadata>::success(metadata);
    }
    if (zwo_response.value->status != 200) {
      return Result<SyncMetadata>::failure(ErrorCode::kTransport, "Intervals workout download failed");
    }
    if (zwo_response.value->body.size() > kMaxWorkoutSourceBytes) return Result<SyncMetadata>::failure(ErrorCode::kBounds, "Intervals workout source is too large");
    const auto parsed = parse_zwo(zwo_response.value->body);
    if (!parsed) return Result<SyncMetadata>::failure(parsed.error->code, parsed.error->message);
    CachedWorkout cached;
    cached.athlete_id = secrets.athlete_id;
    cached.event_id = descriptor.event_id;
    cached.local_date = metadata.local_date;
    cached.timezone = settings_->current().profile.timezone;
    cached.name = descriptor.name.empty() ? parsed.value->name : descriptor.name;
    cached.source_zwo = zwo_response.value->body;
    cached.workout = *parsed.value;
    cached.name = cached.workout.name = cached.name;
    cached.ftp_used_for_preview = settings_->current().profile.ftp_watts;
    cached.downloaded_utc_ms = time.snapshot().utc_ms;
    cached.validated_utc_ms = time.snapshot().utc_ms;
    pending.push_back(std::move(cached));
  }
  for (const auto& cached : pending) {
    const Status saved = cache_->save(cached);
    if (!saved) return Result<SyncMetadata>::failure(saved.error->code, saved.error->message);
  }
  metadata.state = SyncState::kSuccess;
  metadata.last_success_ms = static_cast<std::uint64_t>(time.snapshot().utc_ms);
  return Result<SyncMetadata>::success(metadata);
}

}  // namespace trainer_loop

#else

namespace trainer_loop {
Result<SyncMetadata> IntervalsSync::sync_today(const TimeService&) {
  return Result<SyncMetadata>::failure(ErrorCode::kUnsupported, "Intervals sync requires ESP-IDF");
}
}  // namespace trainer_loop

#endif
