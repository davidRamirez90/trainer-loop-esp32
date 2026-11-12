#include "ui_fixtures.h"

#include <array>
#include <stdexcept>

namespace trainer_loop {
namespace {

UiHistoryEntry history_entry(const char* uuid, const char* name, std::uint32_t duration,
                             const char* status, int watts, int bpm) {
  UiHistoryEntry entry;
  entry.summary.uuid = uuid;
  entry.summary.workout_id = "fixture-workout";
  entry.summary.workout_name = name;
  entry.summary.wall_ms = duration;
  entry.summary.active_ms = duration - 120000U;
  entry.summary.plan_ms = duration - 120000U;
  entry.summary.average_power_w = watts;
  entry.summary.max_power_w = watts + 62;
  entry.summary.average_heart_rate_bpm = bpm;
  entry.summary.max_heart_rate_bpm = bpm + 14;
  entry.summary.distance_cm = 1842000U;
  entry.summary.ascent_cm = 13200;
  entry.upload_status = status;
  return entry;
}

UiSnapshot base(UiScreen screen) {
  UiSnapshot snapshot;
  snapshot.screen = screen;
  snapshot.trainer_status = "connected";
  snapshot.heart_rate_status = "connected";
  snapshot.wifi_status = "connected";
  snapshot.workout_name = "Threshold Builder";
  snapshot.local_date = "2026-09-30";
  snapshot.cache_status = "Plan synced today";
  snapshot.interval_status = "Interval 3 of 6";
  snapshot.today_workout_count = 4U;
  snapshot.today_workout_index = 1U;
  snapshot.interval_index = 2U;
  snapshot.interval_count = 6U;
  snapshot.workout_profile = {36U, 48U, 62U, 74U, 88U, 70U, 52U, 34U};
  snapshot.workout_profile_count = snapshot.workout_profile.size();
  snapshot.interval_elapsed_ms = 90000U;
  snapshot.interval_remaining_ms = 90000U;
  snapshot.workout_progress_percent = 43U;
  snapshot.elapsed_active_ms = 780000U;
  snapshot.elapsed_wall_ms = 840000U;
  snapshot.workout_duration_ms = 1800000U;
  snapshot.target_power_w = 238;
  snapshot.actual_power_w = 241;
  snapshot.heart_rate_bpm = 146;
  snapshot.cadence_rpm = 89.0;
  snapshot.speed_kph = 31.4;
  snapshot.grade_percent = 2.5;
  snapshot.distance_km = 8.6;
  snapshot.relative_ascent_m = 74.0;
  snapshot.profile.ftp_watts = 252;
  snapshot.profile.rider_kg = 74.5;
  snapshot.profile.bike_kg = 8.2;
  snapshot.profile.crr = 0.0045;
  snapshot.profile.cda = 0.31;
  snapshot.settings_available = true;
  snapshot.retry_available = true;
  snapshot.pause_available = true;
  snapshot.resume_available = true;
  snapshot.end_save_available = true;
  snapshot.discard_available = true;
  snapshot.start_available = true;
  snapshot.devices_available = true;
  snapshot.history_available = true;
  snapshot.next_workout_available = true;
  return snapshot;
}

}  // namespace

const std::vector<std::string>& ui_fixture_names() {
  static const std::vector<std::string> names{
      "home-empty", "home-ready", "home-busy", "home-offline",
      "pairing-empty", "pairing-results", "pairing-scanning",
      "preview", "preview-unavailable", "active", "active-no-heart-rate", "active-no-power",
      "paused", "paused-trainer-lost", "stop-confirmation", "discard-confirmation", "completion",
      "history-empty", "history-list", "history-detail", "settings", "error",
      "upload-status", "error-loading", "busy"};
  return names;
}

UiSnapshot ui_fixture(const std::string& name) {
  if (name == "home-empty") {
    UiSnapshot snapshot = base(UiScreen::kHome);
    snapshot.workout_name.clear();
    snapshot.today_workout_count = 0U;
    snapshot.cache_status = "No workout cached";
    snapshot.trainer_status = "disconnected";
    snapshot.heart_rate_status = "disconnected";
    snapshot.wifi_status = "disconnected";
    snapshot.settings_available = false;
    snapshot.history_available = false;
    snapshot.devices_available = false;
    snapshot.retry_available = true;
    snapshot.fault = "Connect Wi-Fi to sync today's plan.";
    return snapshot;
  }
  if (name == "home-ready") return base(UiScreen::kHome);
  if (name == "home-busy" || name == "busy") {
    UiSnapshot snapshot = base(UiScreen::kHome);
    snapshot.busy = true;
    snapshot.fault = "Syncing today's plan";
    return snapshot;
  }
  if (name == "home-offline") {
    UiSnapshot snapshot = base(UiScreen::kHome);
    snapshot.trainer_status = "disconnected";
    snapshot.heart_rate_status = "reconnecting";
    snapshot.wifi_status = "disconnected";
    snapshot.fault = "Using the saved workout plan.";
    return snapshot;
  }
  if (name == "pairing-empty" || name == "pairing-results" || name == "pairing-scanning") {
    UiSnapshot snapshot = base(UiScreen::kPairing);
    snapshot.pairing_scan_active = name == "pairing-scanning";
    snapshot.pairing_scan_pending = name == "pairing-scanning";
    if (name == "pairing-results") {
      snapshot.device_candidates = {
          DeviceIdentity{{0x10, 0x22, 0x34, 0x46, 0x58, 0x6A}, 0U, "Wahoo KICKR", -48, PeripheralRole::kTrainer},
          DeviceIdentity{{0x20, 0x32, 0x44, 0x56, 0x68, 0x7A}, 0U, "Tacx NEO", -62, PeripheralRole::kTrainer},
          DeviceIdentity{{0x30, 0x42, 0x54, 0x66, 0x78, 0x8A}, 0U, "HRM-Pro", -71, PeripheralRole::kHeartRate}};
    }
    return snapshot;
  }
  if (name == "preview" || name == "preview-unavailable") {
    UiSnapshot snapshot = base(UiScreen::kPreview);
    if (name == "preview-unavailable") {
      snapshot.start_available = false;
      snapshot.trainer_status = "disconnected";
      snapshot.fault = "Connect the selected trainer before starting.";
    }
    return snapshot;
  }
  if (name == "active" || name == "active-no-heart-rate" || name == "active-no-power") {
    UiSnapshot snapshot = base(UiScreen::kActive);
    snapshot.pause_available = true;
    if (name == "active-no-heart-rate") {
      snapshot.heart_rate_bpm.reset();
      snapshot.heart_rate_status = "disconnected";
      snapshot.safety_message = "Heart rate sensor disconnected";
    }
    if (name == "active-no-power") {
      snapshot.actual_power_w.reset();
      snapshot.safety_message = "Power sensor data is stale";
    }
    return snapshot;
  }
  if (name == "paused") {
    UiSnapshot snapshot = base(UiScreen::kPaused);
    snapshot.pause_available = false;
    snapshot.busy = false;
    return snapshot;
  }
  if (name == "paused-trainer-lost") {
    UiSnapshot snapshot = base(UiScreen::kPaused);
    snapshot.pause_available = false;
    snapshot.trainer_status = "disconnected";
    snapshot.actual_power_w.reset();
    snapshot.safety_message = "STOP PEDALING; resistance state is unknown";
    return snapshot;
  }
  if (name == "stop-confirmation") {
    UiSnapshot snapshot = base(UiScreen::kStopConfirmation);
    snapshot.pause_available = false;
    return snapshot;
  }
  if (name == "discard-confirmation") {
    UiSnapshot snapshot = base(UiScreen::kDiscardConfirmation);
    snapshot.pause_available = false;
    return snapshot;
  }
  if (name == "completion") {
    UiSnapshot snapshot = base(UiScreen::kCompletion);
    SessionSummary summary = history_entry("fixture-complete", "Threshold Builder", 1800000U,
                                            "UPLOADED", 218, 151).summary;
    summary.complete = true;
    snapshot.completion_summary = summary;
    snapshot.completion_upload_status = "UPLOADED";
    return snapshot;
  }
  if (name == "history-empty" || name == "history-list") {
    UiSnapshot snapshot = base(UiScreen::kHistory);
    if (name == "history-list") {
      snapshot.history_entries = {
          history_entry("fixture-ride-01", "Threshold Builder", 1800000U, "UPLOADED", 218, 151),
          history_entry("fixture-ride-02", "Endurance Ride", 2700000U, "RETRY_WAIT", 176, 137),
          history_entry("fixture-ride-03", "Short Recovery", 1200000U, "PENDING", 118, 122)};
    }
    return snapshot;
  }
  if (name == "history-detail") {
    UiSnapshot snapshot = base(UiScreen::kHistoryDetail);
    snapshot.history_detail = history_entry("fixture-ride-detail", "Threshold Builder",
                                             1800000U, "UPLOADED", 218, 151);
    return snapshot;
  }
  if (name == "settings") {
    UiSnapshot snapshot = base(UiScreen::kSettings);
    snapshot.profile_field = 0U;
    return snapshot;
  }
  if (name == "error") {
    UiSnapshot snapshot = base(UiScreen::kError);
    snapshot.retry_available = true;
    snapshot.busy = false;
    snapshot.fault = "Ride saved locally; upload needs attention.";
    snapshot.completion_upload_status = "AUTH_BLOCKED";
    return snapshot;
  }
  if (name == "error-loading") {
    UiSnapshot snapshot = base(UiScreen::kError);
    snapshot.busy = true;
    snapshot.retry_available = true;
    snapshot.fault = "Saving ride...";
    return snapshot;
  }
  if (name == "upload-status") {
    UiSnapshot snapshot = base(UiScreen::kUploadStatus);
    snapshot.completion_upload_status = "RETRY_WAIT";
    snapshot.fault = "Upload will retry when Wi-Fi is available.";
    return snapshot;
  }
  throw std::invalid_argument("unknown UI fixture: " + name);
}

}  // namespace trainer_loop
