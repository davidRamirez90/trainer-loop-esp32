#include "firmware_app.h"

#include "trainer_loop/fit_encoder.h"
#include "trainer_loop/hash.h"
#include "trainer_loop/limits.h"
#include "trainer_loop/summary.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <utility>

#ifndef ESP_PLATFORM
#include "simulator/platform_services.h"
#endif

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#endif

#ifdef ESP_PLATFORM
#define APP_LOGI(...) ESP_LOGI(__VA_ARGS__)
#define APP_LOGW(...) ESP_LOGW(__VA_ARGS__)
#define APP_LOGE(...) ESP_LOGE(__VA_ARGS__)
#else
#define APP_LOGI(...) ((void)0)
#define APP_LOGW(...) ((void)0)
#define APP_LOGE(...) ((void)0)
#endif

namespace trainer_loop {

namespace {

constexpr std::uint64_t kPairingScanRetryDelayMs = 250U;
constexpr std::uint64_t kAutomaticSyncStartupDelayMs = 20'000U;

std::uint64_t monotonic_ms() {
#ifdef ESP_PLATFORM
  return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
#else
  return HostPlatformServices::instance().monotonic_ms();
#endif
}

void configure_timezone(const ProfileSettings& profile) {
  if (profile.timezone_rule.empty()) return;
  (void)::setenv("TZ", profile.timezone_rule.c_str(), 1);
  ::tzset();
}

void log_runtime_health(const char* stage) {
#ifdef ESP_PLATFORM
  APP_LOGI("trainer_app", "retry stage=%s stack_free=%u internal_free=%u largest=%u dma_free=%u dma_largest=%u",
           stage == nullptr ? "unknown" : stage,
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT)));
#else
  (void)stage;
#endif
}

}  // namespace

FirmwareApp::FirmwareApp(std::string storage_root)
    : storage_root_(std::move(storage_root)),
      time_([] { return monotonic_ms(); }),
      workout_cache_(storage_root_ + "/cache"),
      intervals_sync_(&intervals_http_, &workout_cache_, &settings_),
      journal_(storage_root_ + "/journal"),
      history_(storage_root_ + "/history.db"),
      fit_store_(storage_root_ + "/fit"),
      upload_queue_(storage_root_ + "/upload-queue.db") {
  ble_.set_wait_observer([this](std::uint64_t now_ms) { on_controller_wait(now_ms); });
#ifndef ESP_PLATFORM
  HostPlatformServices::instance().configure(storage_root_);
#endif
}

void FirmwareApp::on_controller_wait(std::uint64_t now_ms) {
  runtime_now_ms_ = std::max(runtime_now_ms_, now_ms);
  trace_event("runtime.wait_tick t=" + std::to_string(now_ms));
  consume_ble_notifications();
  const Status advanced = coordinator_.tick_control_suppressed(now_ms, sensors_);
  if (!advanced) {
    ui_message_ = advanced.error->message;
    trace_event("runtime.wait_tick_error t=" + std::to_string(now_ms) + " error=" +
                advanced.error->message);
  }
}

Result<SyncMetadata> FirmwareApp::sync_today_and_persist() {
  const std::uint32_t generation_before = settings_.current().generation;
  const auto result = intervals_sync_.sync_today(time_);
  last_sync_state_ = result ? result.value->state : SyncState::kError;
  if (result && (last_sync_state_ == SyncState::kSuccess ||
                 last_sync_state_ == SyncState::kEmpty ||
                 last_sync_state_ == SyncState::kAuthBlocked)) {
    ui_message_ = sync_state_label(last_sync_state_);
  }
  if (result && settings_.current().generation != generation_before) {
    const Status persisted = nvs_.save(settings_.current());
    if (!persisted) {
      APP_LOGW("trainer_app", "updated remote profile could not be persisted: %s",
               persisted.error->message.c_str());
    }
  }
  return result;
}

Status FirmwareApp::commit_setup(const SetupCandidate& candidate) {
  APP_LOGI("trainer_setup", "setup validation started");
  const auto prepared = setup_transaction_.prepare(candidate);
  if (!prepared) {
    APP_LOGW("trainer_setup", "setup values rejected: %s", prepared.error->message.c_str());
    return Status::failure(prepared.error->code, prepared.error->message);
  }

  const Status wifi = wifi_.verify_credentials(candidate.secrets.wifi_ssid,
                                               candidate.secrets.wifi_password, 15000U);
  if (!wifi) {
    APP_LOGW("trainer_setup", "setup Wi-Fi verification failed: %s", wifi.error->message.c_str());
    return wifi;
  }
  APP_LOGI("trainer_setup", "setup Wi-Fi verification passed");
  configure_timezone(candidate.profile);
  const Status time = time_sync_.synchronize(&time_, 15000U);
  if (!time) {
    APP_LOGW("trainer_setup", "setup time verification failed: %s", time.error->message.c_str());
    return time;
  }
  APP_LOGI("trainer_setup", "setup time verification passed");

  const auto athlete = intervals_http_.execute(
      build_athlete_request(candidate.secrets.athlete_id, candidate.secrets.api_key));
  if (!athlete) {
    APP_LOGW("trainer_setup", "Intervals athlete request failed: %s", athlete.error->message.c_str());
    return Status::failure(athlete.error->code, athlete.error->message);
  }
  APP_LOGI("trainer_setup", "Intervals athlete verification returned HTTP %d", athlete.value->status);
  if (athlete.value->status == 401 || athlete.value->status == 403) {
    APP_LOGW("trainer_setup", "Intervals credentials were rejected");
    return Status::failure(ErrorCode::kAuthentication, "Intervals credentials were rejected");
  }
  if (athlete.value->status != 200) {
    APP_LOGW("trainer_setup", "Intervals athlete verification returned HTTP %d",
             athlete.value->status);
    return Status::failure(ErrorCode::kTransport,
                           "Intervals athlete verification returned HTTP " +
                               std::to_string(athlete.value->status));
  }

  const Status persisted = nvs_.save(*prepared.value);
  if (!persisted) {
    APP_LOGW("trainer_setup", "settings could not be persisted: %s", persisted.error->message.c_str());
    return persisted;
  }
  const Status committed = setup_transaction_.commit(*prepared.value, true, true, true);
  if (!committed) {
    APP_LOGW("trainer_setup", "settings commit rejected: %s", committed.error->message.c_str());
    return committed;
  }
  APP_LOGI("trainer_setup", "setup settings committed");

  // A cache miss or a temporary cloud failure must not discard valid setup.
  // The prior cache remains authoritative until a complete sync publishes a
  // replacement workout set.
  const auto sync = sync_today_and_persist();
  if (!sync) {
    APP_LOGW("trainer_app", "initial workout sync deferred: %s", sync.error->message.c_str());
  } else {
    APP_LOGI("trainer_app", "initial workout sync state=%u",
             static_cast<unsigned>(sync.value->state));
  }
  APP_LOGI("trainer_setup", "setup validation complete; handoff scheduled");
  return Status::ok();
}

void FirmwareApp::on_provisioning_stopped() {
  if (!settings_.has_committed_settings()) return;
  const Status connected = wifi_.connect(settings_.current().secrets.wifi_ssid,
                                         settings_.current().secrets.wifi_password);
  if (!connected) APP_LOGW("trainer_app", "Wi-Fi reconnect after setup could not start");
  const Status ble_status = ble_.initialize();
  if (!ble_status) {
    APP_LOGE("trainer_app", "BLE could not start after setup: %s", ble_status.error->message.c_str());
    return;
  }
  sync_retry_.request_after(monotonic_ms(), kAutomaticSyncStartupDelayMs);
  APP_LOGI("trainer_app", "automatic workout sync retry armed after setup delay");
  start_runtime_task();
}

namespace {

constexpr std::uint32_t kRuntimePeriodMs = 100U;
#ifdef ESP_PLATFORM
// Intervals sync parses bounded JSON and normalized workouts on this task.
// The previous 8192-word stack reached only 744 words free after a normal
// three-request retry, leaving too little margin for an adjacent UI/retry
// path. Keep the work on the same serialized runtime task, but give it a
// measured safety margin rather than allowing a stack-overflow reset.
constexpr std::uint32_t kRuntimeStackSize = 12288U;
constexpr UBaseType_t kRuntimePriority = 4U;
#endif
constexpr std::uint64_t kFitReservationBytes = 32U;
constexpr std::uint64_t kSessionMetadataReserveBytes = 16U * 1024U;

std::uint64_t session_reservation_bytes() {
  const std::uint64_t samples = static_cast<std::uint64_t>(kMaxSessionSamples);
  const std::uint64_t per_sample = static_cast<std::uint64_t>(kTelemetrySampleBytes) + kFitReservationBytes;
  if (samples > (UINT64_MAX - kSessionMetadataReserveBytes) / per_sample) return UINT64_MAX;
  const std::uint64_t raw = samples * per_sample + kSessionMetadataReserveBytes;
  return raw > (UINT64_MAX / 11U) * 10U ? UINT64_MAX : (raw * 11U) / 10U;
}

bool connected_with_fresh_power(const SensorSnapshot& sensors, std::uint64_t now_ms) {
  return sensors.trainer_link == LinkState::kConnected && sensors.telemetry_subscription &&
         sensors.power_w.fresh(now_ms, kPowerStaleAfterMs);
}

Result<std::vector<std::uint8_t>> read_fit_for_upload(const UploadEntry& entry,
                                                      const std::string& storage_root) {
  const std::string expected_path = storage_root + "/fit/" + entry.session_uuid + ".fit";
  if (entry.fit_path != expected_path) {
    return Result<std::vector<std::uint8_t>>::failure(
        ErrorCode::kInvalidArgument, "upload FIT path is outside the artifact store");
  }
  std::ifstream input(entry.fit_path, std::ios::binary);
  if (!input) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kStorage, "upload FIT artifact is unavailable");
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size <= 0 || static_cast<std::uint64_t>(size) > kMaxFitArtifactBytes) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kBounds, "upload FIT artifact is too large");
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input || sha256_hex(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())) != entry.fit_sha256 ||
      !validate_fit_bytes(bytes)) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kStorage, "upload FIT artifact integrity failed");
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

}  // namespace

#ifdef ESP_PLATFORM
void FirmwareApp::runtime_task_entry(void* argument) {
  auto* app = static_cast<FirmwareApp*>(argument);
  if (app != nullptr) app->runtime_loop();
  vTaskDelete(nullptr);
}
#endif

void FirmwareApp::start_runtime_task() {
#ifdef ESP_PLATFORM
  if (runtime_task_ != nullptr) return;
#else
  if (runtime_started_) return;
#endif
  const RideState state = coordinator_.snapshot().state;
  if (state != RideState::kReady) {
    const Status ready = coordinator_.enter_ready();
    if (!ready) {
      APP_LOGE("trainer_app", "session coordinator could not enter Ready: %s",
               ready.error->message.c_str());
      return;
    }
  }
  runtime_started_ = true;
#ifdef ESP_PLATFORM
  if (xTaskCreate(&FirmwareApp::runtime_task_entry, "trainer_runtime", kRuntimeStackSize,
                  this, kRuntimePriority, &runtime_task_) != pdPASS) {
    runtime_task_ = nullptr;
    runtime_started_ = false;
    const std::size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const std::size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    APP_LOGE("trainer_app", "runtime task could not start (internal free=%u largest=%u stack=%u words)",
             static_cast<unsigned>(free_internal), static_cast<unsigned>(largest_internal),
             static_cast<unsigned>(kRuntimeStackSize));
  } else {
    APP_LOGI("trainer_app", "runtime task started (stack free=%u words)",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(runtime_task_)));
  }
#else
  runtime_started_ = true;
#endif
}

void FirmwareApp::consume_ble_notifications() {
  BleNotification notification;
  while (ble_.pop_notification(&notification)) {
    const ReconnectState& link = ble_.connection_manager().state(notification.role);
    // A packet queued by an old connection must not refresh fields after a
    // reconnect has established a new ownership generation.
    if (!notification_is_current(link, notification.generation)) {
      trace_event("ble.stale role=" + std::to_string(static_cast<unsigned>(notification.role)) +
                  " generation=" + std::to_string(notification.generation));
      continue;
    }
    trace_event("ble.notification kind=" +
                std::to_string(static_cast<unsigned>(notification.kind)) +
                " generation=" + std::to_string(notification.generation));
    if (notification.kind == BleNotificationKind::kIndoorBikeData) {
      const auto parsed = parse_indoor_bike_data(notification.payload.data(), notification.length);
      if (parsed) {
        const Status applied = apply_indoor_bike_data(*parsed.value, notification.timestamp_ms, &sensors_);
        if (!applied) APP_LOGW("trainer_app", "FTMS telemetry was rejected");
      }
    } else if (notification.kind == BleNotificationKind::kHeartRateMeasurement) {
      const auto parsed = parse_heart_rate_measurement(notification.payload.data(), notification.length);
      if (parsed) {
        const Status applied = apply_heart_rate_measurement(*parsed.value, notification.timestamp_ms, &sensors_);
        if (!applied) APP_LOGW("trainer_app", "heart-rate telemetry was rejected");
      }
    }
  }

  const ReconnectState& trainer = ble_.connection_manager().state(PeripheralRole::kTrainer);
  const ReconnectState& heart_rate = ble_.connection_manager().state(PeripheralRole::kHeartRate);
  sensors_.trainer_link = trainer.link;
  sensors_.heart_rate_link = heart_rate.link;
  sensors_.telemetry_subscription = ble_.trainer_services_ready() &&
                                     trainer.link == LinkState::kConnected;
  sensors_.control_granted = ble_.trainer_control_valid();
  if (trainer.link != LinkState::kConnected || !sensors_.telemetry_subscription) {
    sensors_.power_w.valid = false;
    sensors_.cadence_rpm.valid = false;
    sensors_.trainer_speed_kph.valid = false;
    sensors_.distance_m.valid = false;
  }
  if (heart_rate.link != LinkState::kConnected) sensors_.heart_rate_bpm.valid = false;
}

void FirmwareApp::load_cached_workout_if_available(bool refresh) {
  if ((!refresh && coordinator_.selected_workout() != nullptr) ||
      (coordinator_.snapshot().state != RideState::kReady &&
       !(refresh && coordinator_.snapshot().state == RideState::kPreview)) || !time_.trusted()) {
    return;
  }
  const std::string local_date = local_date_for_utc(
      time_.snapshot().utc_ms, settings_.current().profile.timezone_rule);
  if (local_date.empty()) return;
  std::vector<CachedWorkout> cached = workout_cache_.list_for_date(local_date);
  if (cached.empty()) {
    cached_workouts_.clear();
    active_cached_workout_.reset();
    active_cached_workout_index_ = 0U;
    return;
  }
  cached_workouts_ = std::move(cached);
  active_cached_workout_index_ = 0U;
  const Status selected = select_cached_workout(active_cached_workout_index_);
  if (!selected) {
    APP_LOGW("trainer_app", "cached workout could not enter preview: %s",
             selected.error->message.c_str());
  } else if (cached_workouts_.size() > 1U) {
    ui_message_ = "Multiple workouts found — use NEXT";
    APP_LOGI("trainer_app", "%u cached workouts available; previewing 1 of %u",
             static_cast<unsigned>(cached_workouts_.size()),
             static_cast<unsigned>(cached_workouts_.size()));
  }
}

Status FirmwareApp::select_cached_workout(std::size_t index) {
  if (index >= cached_workouts_.size()) {
    return Status::failure(ErrorCode::kBounds, "cached workout selection is out of range");
  }
  active_cached_workout_index_ = index;
  active_cached_workout_ = cached_workouts_[index];
  const Status selected = coordinator_.select_workout(
      active_cached_workout_->workout, settings_.current().profile, true);
  if (!selected) {
    active_cached_workout_.reset();
  } else {
    // A newly selected cached workout is presented first as the compact Today
    // card. The user explicitly opens the detail view from that card.
    workout_preview_active_ = false;
  }
  return selected;
}

UiSnapshot FirmwareApp::make_runtime_ui_snapshot(std::uint64_t now_ms) const {
  UiSnapshot ui = make_ui_snapshot(coordinator_.snapshot(), coordinator_.selected_workout());
  ui.busy = ui_busy_;
  ui.profile = settings_.current().profile;
  ui.wifi_status = wifi_.connected() ? "Wi-Fi connected" : "Wi-Fi offline";
  if (time_.trusted()) {
    ui.local_date = local_date_for_utc(
        time_.snapshot().utc_ms, settings_.current().profile.timezone_rule);
  }
  if (!wifi_.connected()) {
    ui.cache_status = cached_workouts_.empty() ? "Wi-Fi offline" : "Offline · cached";
  } else if (last_sync_state_ == SyncState::kNever || last_sync_state_ == SyncState::kRunning) {
    ui.cache_status = "Wi-Fi online";
  } else {
    ui.cache_status = sync_state_label(last_sync_state_);
  }
  if (!cached_workouts_.empty()) {
    ui.today_workout_index = std::min(active_cached_workout_index_, cached_workouts_.size() - 1U);
    ui.today_workout_count = cached_workouts_.size();
    ui.next_workout_available = cached_workouts_.size() > 1U &&
                                !session_is_active(coordinator_.snapshot().state);
  }
  ui.history_available = history_ready_ && !session_is_active(coordinator_.snapshot().state);
  if (!ui_message_.empty() && (ui_busy_ || !session_is_active(coordinator_.snapshot().state))) {
    ui.fault = ui_message_;
  }
  if (!session_is_active(coordinator_.snapshot().state)) {
    if (coordinator_.selected_workout() == nullptr && ui_message_.empty())
      ui.fault = "No cached workout for today. Connect Wi-Fi and tap Retry.";
  }
  if (ui_busy_ && !ui_message_.empty() && ui.safety_message.empty()) {
    ui.safety_message = ui_message_;
  }
  if (settings_active_) {
    ui.screen = UiScreen::kSettings;
    ui.trainer_status = nvs_.encrypted_at_rest() ? "Settings" : "Development: unencrypted NVS";
    ui.profile = profile_draft_;
    ui.profile_field = profile_field_;
  }
  if (history_ready_) {
    for (const SessionSummary& summary : history_.recent(4U)) {
      UiHistoryEntry entry;
      entry.summary = summary;
      entry.upload_status = "Saved locally";
      for (const UploadEntry& upload : upload_queue_.entries()) {
        if (upload.session_uuid == summary.uuid) {
          entry.upload_status = upload_state_name(upload.state);
          break;
        }
      }
      ui.history_entries.push_back(std::move(entry));
    }
  }
  if (coordinator_.snapshot().state == RideState::kComplete) {
    const auto summary = history_.find(coordinator_.session_uuid());
    if (summary) ui.completion_summary = *summary.value;
    for (const UploadEntry& upload : upload_queue_.entries()) {
      if (upload.session_uuid == coordinator_.session_uuid()) {
        ui.completion_upload_status = upload_state_name(upload.state);
        break;
      }
    }
    if (ui.completion_upload_status.empty()) {
      ui.completion_upload_status = artifacts_finalized_ ? "Upload pending" : "Save pending";
    }
  }
  if (pairing_active_) {
    ui.screen = UiScreen::kPairing;
    ui.devices_available = false;
    ui.history_available = false;
    ui.pairing_scan_active = ble_.connection_manager().scanning();
    ui.pairing_scan_pending = pairing_scan_pending_ && !ui.pairing_scan_active;
    for (std::size_t index = 0U; index < ble_.scan_result_count(); ++index) {
      DeviceIdentity candidate;
      if (ble_.scan_result(index, &candidate)) ui.device_candidates.push_back(std::move(candidate));
    }
  }
  if (history_active_) {
    ui.screen = UiScreen::kHistory;
    ui.devices_available = false;
    ui.history_available = false;
    ui.device_candidates.clear();
  }
  if (history_detail_active_) {
    ui.screen = UiScreen::kHistoryDetail;
    ui.devices_available = false;
    ui.history_available = false;
    ui.device_candidates.clear();
    const auto detail = history_.find(history_detail_uuid_);
    if (detail) {
      UiHistoryEntry entry;
      entry.summary = *detail.value;
      entry.upload_status = "Saved locally";
      for (const UploadEntry& upload : upload_queue_.entries()) {
        if (upload.session_uuid == entry.summary.uuid) {
          entry.upload_status = upload_state_name(upload.state);
          break;
        }
      }
      ui.history_detail = std::move(entry);
    } else {
      ui.fault = "Saved ride details are unavailable";
    }
  }
  if (stop_confirmation_active_) {
    ui = make_stop_confirmation_snapshot(std::move(ui));
  }
  if (discard_confirmation_active_) {
    ui = make_discard_confirmation_snapshot(std::move(ui));
  }
  if (ui.screen == UiScreen::kPreview) {
    ui.start_available = time_.trusted() && capabilities_published_ &&
                         connected_with_fresh_power(sensors_, now_ms) &&
                         check_storage_capacity(storage_root_, session_reservation_bytes()).enough;
    if (!time_.trusted()) ui.safety_message = "Clock needs trusted time before start";
    else if (!capabilities_published_) ui.safety_message = "Trainer capabilities unavailable";
    else if (!connected_with_fresh_power(sensors_, now_ms)) ui.safety_message = "Fresh trainer power required";
    else if (!ui.start_available) ui.safety_message = "Recording storage reserve unavailable";
    if (!workout_preview_active_) {
      // Keep the coordinator in Preview so its selected workout and readiness
      // checks remain intact, while the default surface stays a sparse Today
      // home screen until the user taps the workout card.
      ui.screen = UiScreen::kHome;
    }
  }
  return ui;
}

void FirmwareApp::present_runtime_ui(std::uint64_t now_ms) {
  const Status presented = display_.present(make_runtime_ui_snapshot(now_ms));
  if (!presented) APP_LOGW("trainer_app", "UI snapshot was not presented: %s",
                           presented.error->message.c_str());
}

Status FirmwareApp::retry_connection_and_sync(std::uint64_t now_ms) {
  ui_busy_ = true;
  ui_message_ = "Retrying connection and sync";
  // Retry performs bounded network and time-sync waits on this task. Paint
  // the progress state before entering them so the touch UI never looks
  // like it reverted to its startup screen or stopped responding.
  present_runtime_ui(now_ms);
  log_runtime_health("begin");

  Status status = Status::ok();
  if (!wifi_.connected()) {
    log_runtime_health("before_wifi");
    status = wifi_.connect(settings_.current().secrets.wifi_ssid,
                           settings_.current().secrets.wifi_password);
    if (status) status = wifi_.wait_for_connection(10000U);
    log_runtime_health(status ? "after_wifi" : "wifi_failed");
  }
  if (status) {
    status = time_sync_.synchronize(&time_, 10000U);
    log_runtime_health(status ? "after_time" : "time_failed");
  }
  if (status) {
    const auto sync = sync_today_and_persist();
    if (!sync) status = Status::failure(sync.error->code, sync.error->message);
    else if (sync.value->state == SyncState::kAuthBlocked) {
      status = Status::failure(ErrorCode::kAuthentication,
                               "Intervals credentials rejected; cached workouts kept");
    } else {
      ui_message_ = sync_state_label(sync.value->state);
      load_cached_workout_if_available(true);
    }
    log_runtime_health(status ? "after_sync" : "sync_failed");
  }
  if (upload_queue_ready_) {
    const Status retry = upload_queue_.retry_failed(now_ms);
    if (status && !retry) status = retry;
  }
  next_upload_attempt_ms_ = 0;
  ui_busy_ = false;
  log_runtime_health(status ? "complete" : "failed");
  return status;
}

void FirmwareApp::process_ui_command(const UiCommand& command, std::uint64_t now_ms) {
  trace_event("ui.command t=" + std::to_string(now_ms) + " type=" +
              std::to_string(static_cast<unsigned>(command.type)) + " index=" +
              std::to_string(command.index));
  Status status = Status::ok();
  const RideState ride_state = coordinator_.snapshot().state;
  if (ride_state == RideState::kFinishing && command.type == UiCommandType::kRetry) {
    const Status saved = coordinator_.end_and_save(now_ms);
    ui_message_ = saved ? "Session saved" : saved.error->message;
    return;
  }
  if (session_is_active(ride_state) &&
      command.type != UiCommandType::kPause && command.type != UiCommandType::kResume &&
      command.type != UiCommandType::kEndAndSave && command.type != UiCommandType::kOpenStopConfirmation &&
      command.type != UiCommandType::kOpenDiscardConfirmation && command.type != UiCommandType::kDiscardWorkout &&
      command.type != UiCommandType::kBack) {
    ui_message_ = "Ride active: pause, resume, end, discard, or go back";
    return;
  }
  switch (command.type) {
    case UiCommandType::kOpenWorkoutPreview:
      if (ride_state != RideState::kPreview || coordinator_.selected_workout() == nullptr) {
        status = Status::failure(ErrorCode::kConflict, "workout preview is unavailable");
      } else {
        workout_preview_active_ = true;
        ui_message_.clear();
      }
      break;
    case UiCommandType::kOpenSettings:
      workout_preview_active_ = false;
      settings_active_ = true;
      history_active_ = false;
      history_detail_active_ = false;
      pairing_active_ = false;
      pairing_scan_pending_ = false;
      next_pairing_scan_attempt_ms_ = 0U;
      profile_draft_ = settings_.current().profile;
      profile_field_ = 0;
      ui_message_ = "Adjust values, then Save. Back cancels.";
      break;
    case UiCommandType::kNextWorkout: {
      if (cached_workouts_.size() < 2U) {
        status = Status::failure(ErrorCode::kConflict, "No alternate workout is cached for today");
        break;
      }
      const TodayWorkoutSelection next = next_today_workout(
          {active_cached_workout_index_, cached_workouts_.size()});
      status = select_cached_workout(next.index);
      if (status) {
        workout_preview_active_ = true;
        ui_message_ = "Workout " + std::to_string(next.index + 1U) + " of " +
                      std::to_string(cached_workouts_.size()) + " selected";
      }
      break;
    }
    case UiCommandType::kProfileNext:
      if (settings_active_) profile_field_ = (profile_field_ + 1U) % 5U;
      break;
    case UiCommandType::kProfileDecrease:
    case UiCommandType::kProfileIncrease:
      if (settings_active_) status = adjust_ui_profile(&profile_draft_, profile_field_,
          command.type == UiCommandType::kProfileIncrease ? 1 : -1);
      break;
    case UiCommandType::kSaveProfile:
      if (!settings_active_) break;
      if (coordinator_.selected_workout() != nullptr) {
        status = validate_workout(*coordinator_.selected_workout(), profile_draft_.ftp_watts);
        if (!status) break;
      }
      status = save_ui_profile(settings_, profile_draft_, ride_state,
          [this](const SettingsGeneration& value) { return nvs_.save(value); });
      if (status) {
        if (active_cached_workout_) status = select_cached_workout(active_cached_workout_index_);
        settings_active_ = false;
        ui_message_ = "Profile saved on device";
      }
      break;
    case UiCommandType::kRetry: {
      sync_retry_.request(now_ms);
      if (!sync_retry_.due(now_ms, false, ui_busy_)) break;
      sync_retry_.begin();
      status = retry_connection_and_sync(now_ms);
      sync_retry_.finish(now_ms, static_cast<bool>(status));
      break;
    }
    case UiCommandType::kRetryUploads: {
      if (!upload_queue_ready_) {
        status = Status::failure(ErrorCode::kStorage, "Upload queue is unavailable");
        break;
      }
      status = upload_queue_.retry_failed(now_ms);
      if (status) {
        // The worker normally waits for its one-second pacing interval. A
        // manual retry is an explicit request to try as soon as the network
        // is available, while still preserving ride and Retry-After guards in
        // process_pending_upload().
        next_upload_attempt_ms_ = now_ms;
        ui_message_ = "Upload retry queued";
      }
      break;
    }
    case UiCommandType::kOpenStopConfirmation:
      if (ride_state == RideState::kRunning) {
        status = coordinator_.pause(PauseReason::kUser, now_ms);
      } else if (ride_state != RideState::kPaused) {
        status = Status::failure(ErrorCode::kConflict, "there is no paused ride to end");
      }
      if (status) {
        stop_confirmation_active_ = true;
        discard_confirmation_active_ = false;
        ui_message_ = "Choose END & SAVE or CANCEL";
      }
      break;
    case UiCommandType::kOpenDiscardConfirmation:
      if (ride_state == RideState::kRunning) {
        status = coordinator_.pause(PauseReason::kUser, now_ms);
      } else if (ride_state != RideState::kPaused) {
        status = Status::failure(ErrorCode::kConflict, "there is no active ride to discard");
      }
      if (status) {
        stop_confirmation_active_ = false;
        discard_confirmation_active_ = true;
        ui_message_ = "Choose DISCARD RIDE or KEEP PAUSED";
      }
      break;
    case UiCommandType::kOpenDevices:
      workout_preview_active_ = false;
      history_active_ = false;
      history_detail_active_ = false;
      pairing_active_ = true;
      pairing_scan_pending_ = false;
      next_pairing_scan_attempt_ms_ = 0U;
      ui_message_.clear();
      status = ble_.start_foreground_scan(now_ms);
      if (!status && status.error->code == ErrorCode::kConflict) {
        pairing_scan_pending_ = true;
        next_pairing_scan_attempt_ms_ = now_ms + kPairingScanRetryDelayMs;
        ui_message_ = "Waiting for device setup; scan will start automatically";
        status = Status::ok();
      }
      break;
    case UiCommandType::kOpenHistory:
      if (coordinator_.snapshot().state == RideState::kRunning ||
          coordinator_.snapshot().state == RideState::kPaused) {
        status = Status::failure(ErrorCode::kConflict, "history is unavailable during a ride");
      } else if (!history_ready_) {
        status = Status::failure(ErrorCode::kStorage, "history catalog is unavailable");
      } else {
        workout_preview_active_ = false;
        pairing_active_ = false;
        pairing_scan_pending_ = false;
        next_pairing_scan_attempt_ms_ = 0U;
        history_active_ = true;
        history_detail_active_ = false;
      }
      break;
    case UiCommandType::kOpenHistoryEntry: {
      const std::vector<SessionSummary> entries = history_.recent(4U);
      if (command.index >= entries.size()) {
        status = Status::failure(ErrorCode::kInvalidArgument, "saved ride row is no longer available");
        break;
      }
      pairing_active_ = false;
      workout_preview_active_ = false;
      history_active_ = false;
      history_detail_active_ = true;
      history_detail_uuid_ = entries[command.index].uuid;
      break;
    }
    case UiCommandType::kSelectDevice: {
      DeviceIdentity candidate;
      if (!ble_.scan_result(command.index, &candidate)) {
        status = Status::failure(ErrorCode::kInvalidArgument, "BLE scan candidate is no longer available");
        break;
      }
      status = ble_.select_device(candidate);
      if (status && settings_.has_committed_settings()) {
        status = settings_.update_device(candidate.role, candidate, settings_.current().generation);
        if (status) status = nvs_.save(settings_.current());
      }
      if (status) {
        pairing_active_ = false;
        pairing_scan_pending_ = false;
        next_pairing_scan_attempt_ms_ = 0U;
        capabilities_published_ = false;
      }
      break;
    }
    case UiCommandType::kStart: {
      const TimeSnapshot& time_snapshot = time_.snapshot();
      const StartTimeAnchor anchor{time_snapshot.utc_ms, time_snapshot.trusted,
                                  static_cast<std::uint8_t>(time_snapshot.quality)};
      const StorageCapacity capacity = check_storage_capacity(
          storage_root_, session_reservation_bytes());
      status = coordinator_.start(now_ms, anchor, sensors_, capacity.enough,
                                  settings_.current().secrets.athlete_id);
      if (status) {
        artifacts_finalized_ = false;
        workout_preview_active_ = false;
      }
      break;
    }
    case UiCommandType::kPause:
      status = coordinator_.pause(PauseReason::kUser, now_ms);
      break;
    case UiCommandType::kResume:
      status = coordinator_.resume(now_ms, sensors_);
      break;
    case UiCommandType::kEndAndSave:
      if (ride_state == RideState::kComplete) {
        ui_message_ = "Workout already saved";
        break;
      }
      if (!stop_confirmation_active_) {
        status = Status::failure(ErrorCode::kConflict, "confirm END & SAVE before finishing");
        break;
      }
      ui_busy_ = true;
      ui_message_ = "Saving ride...";
      present_runtime_ui(now_ms);
      status = coordinator_.end_and_save(now_ms);
      stop_confirmation_active_ = false;
      break;
    case UiCommandType::kDiscardWorkout:
      if (!discard_confirmation_active_) {
        status = Status::failure(ErrorCode::kConflict, "confirm DISCARD RIDE before leaving");
        break;
      }
      if (ride_state == RideState::kComplete) {
        status = Status::failure(ErrorCode::kConflict, "saved rides cannot be discarded");
        break;
      }
      ui_busy_ = true;
      ui_message_ = "Discarding ride...";
      present_runtime_ui(now_ms);
      status = coordinator_.discard(now_ms);
      if (status) {
        discard_confirmation_active_ = false;
        workout_preview_active_ = false;
        artifacts_finalized_ = false;
        ui_message_.clear();
      }
      break;
    case UiCommandType::kBack:
      if (discard_confirmation_active_) {
        discard_confirmation_active_ = false;
        ui_message_ = "Ride paused";
        break;
      }
      if (stop_confirmation_active_) {
        stop_confirmation_active_ = false;
        ui_message_ = "Ride paused";
        break;
      }
      if (settings_active_) { settings_active_ = false; ui_message_.clear(); break; }
      if (pairing_active_) {
        status = ble_.stop_scan();
        pairing_active_ = false;
        pairing_scan_pending_ = false;
        next_pairing_scan_attempt_ms_ = 0U;
        ui_message_.clear();
        break;
      }
      if (history_detail_active_) {
        history_detail_active_ = false;
        history_active_ = true;
        break;
      }
      if (history_active_) {
        history_active_ = false;
        break;
      }
      if (workout_preview_active_ && ride_state == RideState::kPreview) {
        workout_preview_active_ = false;
        ui_message_.clear();
        break;
      }
      if (coordinator_.snapshot().state == RideState::kComplete) {
        if (!artifacts_finalized_) {
          status = Status::failure(ErrorCode::kStorage, "Local ride files still pending; retry before leaving");
          break;
        }
        status = coordinator_.enter_ready();
        cached_workouts_.clear();
        active_cached_workout_index_ = 0U;
        active_cached_workout_.reset();
        capabilities_published_ = false;
        history_detail_active_ = false;
        history_detail_uuid_.clear();
        stop_confirmation_active_ = false;
        workout_preview_active_ = false;
      }
      break;
    default:
      break;
  }
  if (status && coordinator_.snapshot().state == RideState::kComplete &&
      !artifacts_finalized_) {
    status = finalize_session_artifacts();
  }
  if (command.type == UiCommandType::kEndAndSave) {
    if (status && coordinator_.snapshot().state == RideState::kComplete && artifacts_finalized_) {
      ui_message_ = "Workout saved";
    }
    ui_busy_ = false;
  }
  if (command.type == UiCommandType::kDiscardWorkout) {
    ui_busy_ = false;
  }
  if (!status) {
    ui_message_ = status.error->message;
    APP_LOGW("trainer_app", "UI action %u was not applied: %s",
             static_cast<unsigned>(command.type), status.error->message.c_str());
  }
}

Status FirmwareApp::publish_session_artifacts(const JournalManifest& manifest, const Workout& workout,
                                              std::vector<TelemetrySample> samples,
                                              std::vector<SessionEvent> events) {
  if (!history_ready_ || !fit_store_ready_ || !upload_queue_ready_) {
    return Status::failure(ErrorCode::kStorage, "session artifact storage is unavailable");
  }
  FitInput input;
  input.start_utc_ms = manifest.start_utc_ms;
  input.time_trusted = manifest.time_trusted;
  input.wall_duration_ms = manifest.wall_ms;
  input.active_duration_ms = manifest.active_ms;
  input.samples = std::move(samples);
  input.events = std::move(events);
  const auto encoded = encode_fit(input);
  if (!encoded) return Status::failure(encoded.error->code, encoded.error->message);
  const auto artifact = fit_store_.publish(manifest.uuid, *encoded.value);
  if (!artifact) return Status::failure(artifact.error->code, artifact.error->message);

  SessionSummary summary = summarize_session(
      manifest.uuid, workout, input.samples, input.wall_duration_ms, input.active_duration_ms,
      manifest.complete, manifest.interrupted);
  summary.fit_path = artifact.value->path;
  summary.fit_sha256 = artifact.value->sha256;
  const Status history = history_.save(summary);
  if (!history) return history;

  UploadEntry upload;
  upload.session_uuid = manifest.uuid;
  upload.fit_path = artifact.value->path;
  upload.fit_sha256 = artifact.value->sha256;
  upload.athlete_id = manifest.destination_athlete_id.empty()
                          ? settings_.current().secrets.athlete_id
                          : manifest.destination_athlete_id;
  upload.filename = sanitized_fit_filename(upload.session_uuid);
  const Status queued = upload_queue_.enqueue(upload);
  if (!queued) return queued;
  const Status ready = upload_queue_.mark_fit_ready(upload.session_uuid);
  if (!ready) {
    const auto found = std::find_if(upload_queue_.entries().begin(), upload_queue_.entries().end(),
                                    [&](const UploadEntry& entry) {
                                      return entry.session_uuid == upload.session_uuid;
                                    });
    if (found == upload_queue_.entries().end() || found->fit_sha256 != upload.fit_sha256) return ready;
  }
  return Status::ok();
}

Status FirmwareApp::publish_recovered_session_artifacts(RecoveredSession recovered) {
  Workout workout;
  workout.id = recovered.manifest.workout_id.empty() ? recovered.manifest.uuid : recovered.manifest.workout_id;
  workout.name = recovered.manifest.workout_name;
  return publish_session_artifacts(recovered.manifest, workout,
                                   std::move(recovered.samples), std::move(recovered.events));
}

Status FirmwareApp::finalize_session_artifacts() {
  if (artifacts_finalized_) return Status::ok();
  if (coordinator_.snapshot().state != RideState::kComplete ||
      coordinator_.session_uuid().empty() || coordinator_.selected_workout() == nullptr) {
    return Status::failure(ErrorCode::kConflict, "completed session is not available");
  }
  const auto recovered = journal_.read_session(coordinator_.session_uuid());
  if (!recovered) return Status::failure(recovered.error->code, recovered.error->message);
  const Status published = publish_session_artifacts(
      recovered.value->manifest, *coordinator_.selected_workout(),
      std::move(recovered.value->samples), std::move(recovered.value->events));
  if (published) artifacts_finalized_ = true;
  return published;
}

void FirmwareApp::reconcile_finalized_sessions() {
  if (!history_ready_ || !fit_store_ready_ || !upload_queue_ready_) return;
  for (const std::string& uuid : journal_.list_sessions()) {
    const auto history = history_.find(uuid);
    const auto upload = std::find_if(upload_queue_.entries().begin(), upload_queue_.entries().end(),
                                      [&](const UploadEntry& entry) { return entry.session_uuid == uuid; });
    if (history && upload != upload_queue_.entries().end()) continue;

    const auto recovered = journal_.read_session(uuid);
    if (!recovered) {
      APP_LOGW("trainer_app", "finalized session %s could not be read for publication: %s",
               uuid.c_str(), recovered.error->message.c_str());
      continue;
    }
    const Status published = publish_recovered_session_artifacts(*recovered.value);
    if (!published) {
      // Keep the immutable journal session in place. The same reconciliation
      // path retries it on the next boot or whenever runtime storage recovers.
      APP_LOGW("trainer_app", "finalized session %s publication deferred: %s",
               uuid.c_str(), published.error->message.c_str());
    }
  }
}

void FirmwareApp::reconcile_pending_fits() {
  if (pending_fits_reconciled_) return;
  pending_fits_reconciled_ = true;
  if (!upload_queue_ready_) return;
  std::vector<std::string> pending;
  for (const UploadEntry& entry : upload_queue_.entries()) {
    if (entry.state == UploadState::kFitPending) pending.push_back(entry.session_uuid);
  }
  for (const std::string& uuid : pending) {
    const auto found = std::find_if(upload_queue_.entries().begin(), upload_queue_.entries().end(),
                                    [&](const UploadEntry& entry) { return entry.session_uuid == uuid; });
    if (found == upload_queue_.entries().end()) continue;
    const auto bytes = read_fit_for_upload(*found, storage_root_);
    if (bytes) {
      const Status ready = upload_queue_.mark_fit_ready(uuid);
      if (!ready) APP_LOGW("trainer_app", "pending FIT could not be made upload-ready");
    }
  }
}

void FirmwareApp::process_pending_upload(std::uint64_t now_ms) {
  if (now_ms < next_upload_attempt_ms_ || !wifi_.connected() ||
      !settings_.has_committed_settings() || !upload_queue_ready_) return;
  const bool ride_running = session_is_active(coordinator_.snapshot().state);
  const auto due = upload_queue_.next_due(now_ms, ride_running, wifi_.connected());
  if (!due) return;
  const auto bytes = read_fit_for_upload(*due, storage_root_);
  if (!bytes) {
    const Status marked = upload_queue_.mark_uploading(due->session_uuid, now_ms);
    if (marked) {
      (void)upload_queue_.apply_result(
          due->session_uuid, classify_upload_response(0, ""), now_ms);
    }
    next_upload_attempt_ms_ = now_ms + 1000U;
    return;
  }
  const Status marked = upload_queue_.mark_uploading(due->session_uuid, now_ms);
  if (!marked) return;
  const HttpRequest request = build_upload_request(
      due->athlete_id, settings_.current().secrets.api_key, due->session_uuid,
      std::string(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size()));
  const auto response = intervals_http_.execute(request);
  const UploadHttpResult result = response
      ? classify_upload_response(response.value->status, response.value->body,
                                 response.value->retry_after_s)
      : classify_upload_response(0, "");
  const Status applied = upload_queue_.apply_result(due->session_uuid, result, now_ms);
  if (!applied) APP_LOGW("trainer_app", "upload result could not be persisted");
  next_upload_attempt_ms_ = now_ms + 1000U;
}

#ifdef ESP_PLATFORM
void FirmwareApp::runtime_loop() {
  while (true) {
    (void)runtime_step(monotonic_ms());
    vTaskDelay(pdMS_TO_TICKS(kRuntimePeriodMs));
  }
}
#endif

Status FirmwareApp::runtime_step(std::uint64_t now_ms) {
#ifndef ESP_PLATFORM
  auto& platform = HostPlatformServices::instance();
  if (platform.monotonic_ms() < now_ms) platform.set_monotonic_ms(now_ms);
#endif
  if (!runtime_started_) {
    if (setup_active_) return Status::ok();
    return Status::failure(ErrorCode::kConflict, "runtime is not started");
  }
  const std::uint64_t effective_now_ms = std::max(now_ms, monotonic_ms());
  return runtime_iteration(effective_now_ms);
}

Status FirmwareApp::runtime_iteration(std::uint64_t now_ms) {
  runtime_now_ms_ = now_ms;
    trace_event("runtime.step t=" + std::to_string(now_ms));
    reconcile_pending_fits();
    const Status ble_status = ble_.poll(now_ms);
    if (!ble_status) APP_LOGW("trainer_app", "BLE poll deferred: %s", ble_status.error->message.c_str());
    consume_ble_notifications();

    if (!capabilities_published_ && ble_.trainer_services_ready() &&
        coordinator_.snapshot().state != RideState::kRunning &&
        coordinator_.snapshot().state != RideState::kPaused) {
      const Status capabilities = coordinator_.set_trainer_capabilities(ble_.trainer_capabilities());
      if (capabilities) capabilities_published_ = true;
      else APP_LOGW("trainer_app", "trainer capabilities are not ride-ready: %s",
                    capabilities.error->message.c_str());
    }
    load_cached_workout_if_available();

    while (const auto command = display_.pop_command()) {
      process_ui_command(*command, now_ms);
    }
    now_ms = std::max(now_ms, monotonic_ms());
    const std::uint64_t input_now_ms = now_ms;
    const ButtonGestureEvent boot_event = boot_button_.update(
        board_.boot_pressed(), input_now_ms);
    if (boot_event == ButtonGestureEvent::kShortPress) {
      trace_event("boot.short_press t=" + std::to_string(input_now_ms));
      const UiCommandType command = coordinator_.snapshot().state == RideState::kRunning
                                        ? UiCommandType::kPause
                                        : UiCommandType::kBack;
      process_ui_command({command, 0U}, input_now_ms);
    } else if (boot_event == ButtonGestureEvent::kLongPress) {
      // GPIO0 is also the boot strap. Runtime long presses have no reset,
      // erase, or provisioning meaning until a separately validated gesture
      // policy exists; ignoring them keeps a rider-safe boundary.
      APP_LOGW("trainer_app", "BOOT long press ignored during runtime");
      trace_event("boot.long_press_ignored t=" + std::to_string(input_now_ms));
    }
    if (pairing_active_ && pairing_scan_pending_ &&
        !ble_.connection_manager().scanning() && now_ms >= next_pairing_scan_attempt_ms_) {
      const Status scan = ble_.start_foreground_scan(now_ms);
      if (scan) {
        pairing_scan_pending_ = false;
        next_pairing_scan_attempt_ms_ = 0U;
        ui_message_.clear();
      } else if (scan.error->code == ErrorCode::kConflict) {
        next_pairing_scan_attempt_ms_ = now_ms + kPairingScanRetryDelayMs;
      } else {
        pairing_scan_pending_ = false;
        next_pairing_scan_attempt_ms_ = 0U;
        ui_message_ = scan.error->message;
        APP_LOGW("trainer_app", "automatic BLE scan retry failed: %s",
                 scan.error->message.c_str());
      }
    }
    const RideState state_before_auto_retry = coordinator_.snapshot().state;
    if (!ble_.connection_manager().network_sync_blocked() &&
        sync_retry_.due(now_ms, session_is_active(state_before_auto_retry), ui_busy_)) {
      APP_LOGI("trainer_app", "automatic workout sync retry starting (failure=%u)",
               static_cast<unsigned>(sync_retry_.failures()));
      sync_retry_.begin();
      const Status retry = retry_connection_and_sync(now_ms);
      sync_retry_.finish(now_ms, static_cast<bool>(retry));
      if (!retry) {
        ui_message_ = retry.error->message;
        APP_LOGW("trainer_app", "automatic workout sync retry failed: %s",
                 retry.error->message.c_str());
      }
    }
    now_ms = std::max(now_ms, monotonic_ms());
    const Status tick = coordinator_.tick(now_ms, sensors_);
    now_ms = std::max(now_ms, monotonic_ms());
    if (!tick) APP_LOGW("trainer_app", "session tick fault: %s", tick.error->message.c_str());
    if (coordinator_.snapshot().state == RideState::kComplete && !artifacts_finalized_) {
      const Status artifacts = finalize_session_artifacts();
      if (!artifacts) APP_LOGW("trainer_app", "session artifacts deferred: %s", artifacts.error->message.c_str());
    }
    process_pending_upload(now_ms);

    runtime_now_ms_ = now_ms;
    present_runtime_ui(now_ms);
  return Status::ok();
}

void FirmwareApp::start() {
  startup_error_.clear();
#ifdef ESP_PLATFORM
  APP_LOGI("trainer_app", "reset reason=%d", static_cast<int>(esp_reset_reason()));
#endif
  const Status nvs_status = nvs_.initialize();
  if (!nvs_status) {
    startup_error_ = nvs_status.error->message;
    APP_LOGE("trainer_app", "settings storage unavailable: %s", nvs_status.error->message.c_str());
    return;
  }
  const Status settings_status = nvs_.load(&settings_);
  if (!settings_status) {
    startup_error_ = settings_status.error->message;
    APP_LOGE("trainer_app", "persisted settings are invalid: %s", settings_status.error->message.c_str());
    return;
  }
  const Status storage_status = storage_.mount();
  if (!storage_status) {
    startup_error_ = storage_status.error->message;
    APP_LOGE("trainer_app", "recording storage unavailable: %s", storage_status.error->message.c_str());
    return;
  }
  const Status journal_status = journal_.initialize();
  if (!journal_status) {
    startup_error_ = journal_status.error->message;
    APP_LOGE("trainer_app", "session journal unavailable: %s", journal_status.error->message.c_str());
    return;
  }
  std::optional<RecoveredSession> recovered_interrupted;
  if (journal_.has_current_session()) {
    const auto recovered = journal_.recover_current();
    if (!recovered) {
      startup_error_ = recovered.error->message;
      // Keep the current directory intact for later forensic recovery. A
      // corrupt manifest must never be replaced or auto-formatted at boot.
      APP_LOGE("trainer_app", "interrupted session could not be recovered: %s",
               recovered.error->message.c_str());
      return;
    }
    recovered_interrupted = std::move(*recovered.value);
    recovered_interrupted->manifest.complete = false;
    recovered_interrupted->manifest.interrupted = true;
  }
  const Status history_status = history_.load();
  history_ready_ = static_cast<bool>(history_status);
  if (!history_status) {
    APP_LOGW("trainer_app", "history catalog unavailable: %s", history_status.error->message.c_str());
  }
  const Status fit_store_status = fit_store_.initialize();
  fit_store_ready_ = static_cast<bool>(fit_store_status);
  if (!fit_store_status) {
    APP_LOGW("trainer_app", "FIT artifact store unavailable: %s", fit_store_status.error->message.c_str());
  }
  // Retry deadlines are relative waits on disk and must be rebased into this
  // boot's monotonic epoch before the worker considers them due.
  const Status upload_queue_status = upload_queue_.load(monotonic_ms());
  upload_queue_ready_ = static_cast<bool>(upload_queue_status);
  if (!upload_queue_status) {
    APP_LOGW("trainer_app", "upload queue unavailable: %s", upload_queue_status.error->message.c_str());
  }
  if (recovered_interrupted.has_value()) {
    if (!history_ready_ || !fit_store_ready_ || !upload_queue_ready_) {
      startup_error_ = "recovered session artifact storage is unavailable";
      APP_LOGE("trainer_app", "recovered session artifacts cannot be published while storage services are unavailable");
      return;
    }
    const JournalManifest interrupted_manifest = recovered_interrupted->manifest;
    const std::size_t recovered_sample_count = recovered_interrupted->samples.size();
    const Status artifacts = publish_recovered_session_artifacts(std::move(*recovered_interrupted));
    if (!artifacts) {
      startup_error_ = artifacts.error->message;
      APP_LOGE("trainer_app", "recovered session artifacts could not be published: %s",
               artifacts.error->message.c_str());
      return;
    }
    const Status published = journal_.finalize(interrupted_manifest);
    if (!published) {
      startup_error_ = published.error->message;
      APP_LOGE("trainer_app", "recovered session could not be preserved: %s",
               published.error->message.c_str());
      return;
    }
    APP_LOGW("trainer_app", "preserved interrupted session with %u valid samples",
             static_cast<unsigned>(recovered_sample_count));
  }
  reconcile_finalized_sessions();
  const Status cache_status = workout_cache_.initialize();
  if (!cache_status) {
    startup_error_ = cache_status.error->message;
    APP_LOGE("trainer_app", "workout cache unavailable: %s", cache_status.error->message.c_str());
    return;
  }
  const Status board_status = board_.initialize();
  if (!board_status) {
    startup_error_ = board_status.error->message;
    APP_LOGW("trainer_app", "platform foundation is gated: %s", board_status.error->message.c_str());
    return;
  }
  board_v1::RtcReading rtc;
  const Status rtc_status = board_.read_rtc(&rtc);
  if (rtc_status && rtc.valid) {
    const Status time_status = time_.set_from_rtc(rtc.utc_ms, !rtc.oscillator_stopped);
    if (!time_status) {
      time_.invalidate();
      APP_LOGW("trainer_app", "RTC time is outside trusted bounds");
    }
  } else {
    time_.invalidate();
    if (rtc_status.error.has_value()) {
      APP_LOGW("trainer_app", "trusted RTC time unavailable: %s", rtc_status.error->message.c_str());
    }
  }
#if defined(TRAINER_LOOP_HOST_SIM)
  const Status display_status = display_.initialize_host();
#else
  const Status display_status = display_.initialize(board_.display_panel(),
                                                     board_.display_io(), &board_);
#endif
  const Status wifi_status = wifi_.initialize();
  if (!display_status || !wifi_status) {
    startup_error_ = !display_status ? display_status.error->message : wifi_status.error->message;
    APP_LOGE("trainer_app", "runtime subsystem initialization failed");
    return;
  }
  provisioning_.set_commit_handler([this](const SetupCandidate& candidate) {
    return commit_setup(candidate);
  });
  provisioning_.set_stop_handler([this]() { on_provisioning_stopped(); });

  if (!settings_.has_committed_settings()) {
    const Status setup = provisioning_.start(monotonic_ms());
    if (!setup) {
      startup_error_ = setup.error->message;
      APP_LOGE("trainer_app", "protected setup AP could not start: %s", setup.error->message.c_str());
      return;
    }
    const Status setup_display = display_.show_setup(provisioning_.credentials());
    if (!setup_display) {
      startup_error_ = setup_display.error->message;
      APP_LOGE("trainer_app", "setup credentials could not be displayed: %s",
               setup_display.error->message.c_str());
      return;
    }
    setup_active_ = true;
    APP_LOGI("trainer_app", "unprovisioned device awaiting setup at %s",
             provisioning_.credentials().url.c_str());
    return;
  }

  configure_timezone(settings_.current().profile);
  const Status ble_status = ble_.initialize();
  if (!ble_status) {
    startup_error_ = ble_status.error->message;
    APP_LOGE("trainer_app", "BLE initialization failed: %s", ble_status.error->message.c_str());
    return;
  }
  if (settings_.current().trainer_device.has_value()) {
    const Status selected = ble_.select_device(*settings_.current().trainer_device);
    if (!selected) APP_LOGW("trainer_app", "saved trainer identity could not be restored");
  }
  if (settings_.current().heart_rate_device.has_value()) {
    const Status selected = ble_.select_device(*settings_.current().heart_rate_device);
    if (!selected) APP_LOGW("trainer_app", "saved heart-rate identity could not be restored");
  }
  APP_LOGI("trainer_app", "TrainerLoop foundation started with durable storage");
  const Status ready = coordinator_.enter_ready();
  if (!ready) {
    startup_error_ = ready.error->message;
    APP_LOGE("trainer_app", "session coordinator could not enter Ready: %s",
             ready.error->message.c_str());
    return;
  }
  start_runtime_task();
  // Reach the ready UI before any Wi-Fi/SNTP/HTTPS wait. The runtime task
  // performs the same bounded sync sequence in the background, preserving
  // cached/offline riding while avoiding a long startup splash screen.
  sync_retry_.request_after(monotonic_ms(), kAutomaticSyncStartupDelayMs);
  APP_LOGI("trainer_app", "automatic workout sync armed after startup delay");
  if (runtime_started_) (void)runtime_step(monotonic_ms());
}

void FirmwareApp::boot_input(bool pressed) {
#ifndef ESP_PLATFORM
  HostPlatformServices::instance().set_boot_pressed(pressed);
#else
  (void)pressed;
#endif
}

#if defined(TRAINER_LOOP_HOST_SIM)
Status FirmwareApp::set_simulated_utc(std::int64_t utc_ms, bool trusted) {
  HostPlatformServices::instance().set_utc_ms(utc_ms);
  if (!trusted) {
    time_.invalidate();
    return Status::ok();
  }
  return time_.set_from_rtc(utc_ms, true);
}
#endif

Status FirmwareApp::setup(const SetupCandidate& candidate) {
  if (!provisioning_.running() || !provisioning_.commit_handler()) {
    return Status::failure(ErrorCode::kConflict, "setup provisioning is not active");
  }
  const Status committed = provisioning_.commit_handler()(candidate);
  if (!committed) return committed;
  setup_active_ = false;
  const Status stopped = provisioning_.stop();
  if (!stopped) return stopped;
  return Status::ok();
}

UiSnapshot FirmwareApp::snapshot() const {
  UiSnapshot ui = make_runtime_ui_snapshot(runtime_now_ms_);
  if (setup_active_ || provisioning_.running()) {
    ui.screen = UiScreen::kSetup;
  } else if (!runtime_started_ && !startup_error_.empty()) {
    ui.screen = UiScreen::kError;
    ui.fault = startup_error_;
  }
  return ui;
}

void FirmwareApp::trace_event(std::string event) {
#if defined(TRAINER_LOOP_HOST_SIM)
  constexpr std::size_t kTraceCapacity = 4096U;
  std::ofstream output(storage_root_ + "/../events.jsonl", std::ios::app);
  if (output) {
    output << "{\"source\":\"firmware\",\"event\":\"";
    for (const unsigned char c : event) {
      switch (c) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
          if (c < 0x20U) {
            constexpr char kHex[] = "0123456789abcdef";
            output << "\\u00" << kHex[c >> 4U] << kHex[c & 0x0fU];
          } else {
            output << static_cast<char>(c);
          }
      }
    }
    output << "\"}\n";
  }
  if (event_trace_.size() == kTraceCapacity) event_trace_.erase(event_trace_.begin());
  event_trace_.push_back(std::move(event));
#else
  (void)event;
#endif
}

}  // namespace trainer_loop
