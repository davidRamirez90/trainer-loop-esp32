#pragma once

#include "board_v1.h"
#include "ble_nimble.h"
#include "trainer_loop/fit_store.h"
#include "trainer_loop/retry_policy.h"
#include "trainer_loop/history.h"
#include "intervals_http.h"
#include "intervals_sync.h"
#include "lvgl_presenter.h"
#include "provisioning.h"
#include "settings_nvs.h"
#include "storage_mount.h"
#include "trainer_loop/time_service.h"
#include "trainer_loop/upload_queue.h"
#include "trainer_loop/workout_cache.h"
#include "trainer_loop/ui.h"
#include "time_sync.h"
#include "wifi_station.h"
#include "trainer_loop/button_gesture.h"

#include <cstdint>
#include <optional>
#include <vector>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace trainer_loop {

class FirmwareApp {
 public:
  void start();

 private:
  Status commit_setup(const SetupCandidate& candidate);
  void on_provisioning_stopped();
  void start_runtime_task();
  void runtime_loop();
  void consume_ble_notifications();
  void process_ui_command(const UiCommand& command, std::uint64_t now_ms);
  UiSnapshot make_runtime_ui_snapshot(std::uint64_t now_ms) const;
  void present_runtime_ui(std::uint64_t now_ms);
  Status retry_connection_and_sync(std::uint64_t now_ms);
  void load_cached_workout_if_available(bool refresh = false);
  Status select_cached_workout(std::size_t index);
  Result<SyncMetadata> sync_today_and_persist();
  Status publish_session_artifacts(const JournalManifest& manifest, const Workout& workout,
                                   std::vector<TelemetrySample> samples,
                                   std::vector<SessionEvent> events);
  Status publish_recovered_session_artifacts(RecoveredSession recovered);
  Status finalize_session_artifacts();
  void reconcile_finalized_sessions();
  void process_pending_upload(std::uint64_t now_ms);
  void reconcile_pending_fits();

  board_v1::BoardV1 board_;
  TimeService time_;
  StorageMount storage_;
  NvsSettingsBackend nvs_;
  SettingsStore settings_;
  SetupSession setup_session_;
  SetupTransaction setup_transaction_{&settings_};
  ProvisioningServer provisioning_{&setup_session_};
  IntervalsHttpClient intervals_http_;
  WorkoutCache workout_cache_{"/storage/cache"};
  IntervalsSync intervals_sync_{&intervals_http_, &workout_cache_, &settings_};
  TimeSynchronizer time_sync_;
  LvglPresenter display_;
  NimbleCentral ble_;
  SessionJournal journal_{"/storage/journal"};
  HistoryStore history_{"/storage/history.db"};
  FitArtifactStore fit_store_{"/storage/fit"};
  UploadQueue upload_queue_{"/storage/upload-queue.db"};
  SessionCoordinator coordinator_{&ble_, &journal_};
  SensorSnapshot sensors_;
  std::vector<CachedWorkout> cached_workouts_;
  std::size_t active_cached_workout_index_ = 0U;
  std::optional<CachedWorkout> active_cached_workout_;
  bool capabilities_published_ = false;
  bool pairing_active_ = false;
  bool pairing_scan_pending_ = false;
  bool history_active_ = false;
  bool history_detail_active_ = false;
  bool stop_confirmation_active_ = false;
  std::string history_detail_uuid_;
  bool settings_active_ = false;
  bool ui_busy_ = false;
  ProfileSettings profile_draft_;
  std::size_t profile_field_ = 0U;
  std::string ui_message_;
  bool artifacts_finalized_ = false;
  bool pending_fits_reconciled_ = false;
  bool history_ready_ = false;
  bool fit_store_ready_ = false;
  bool upload_queue_ready_ = false;
  std::uint64_t next_upload_attempt_ms_ = 0U;
  std::uint64_t next_pairing_scan_attempt_ms_ = 0U;
  SyncState last_sync_state_ = SyncState::kNever;
  RetryPolicy sync_retry_;
  WifiStation wifi_;
  ButtonGesture boot_button_;
#ifdef ESP_PLATFORM
  static void runtime_task_entry(void* argument);
  TaskHandle_t runtime_task_ = nullptr;
#endif
};

}  // namespace trainer_loop
