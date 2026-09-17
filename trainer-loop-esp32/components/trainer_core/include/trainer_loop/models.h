#pragma once

#include "trainer_loop/limits.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trainer_loop {

enum class SegmentKind : std::uint8_t { kStep = 0, kRamp = 1 };
enum class PowerOrigin : std::uint8_t { kFractionFtp = 0, kAbsoluteWatts = 1 };

struct CadenceRange {
  double low_rpm = 0.0;
  double high_rpm = 0.0;
};

struct WorkoutSegment {
  std::uint32_t duration_ms = 0U;
  std::string id;
  std::string name;
  std::string phase;
  SegmentKind kind = SegmentKind::kStep;
  PowerOrigin power_origin = PowerOrigin::kFractionFtp;

  // For a step, start and end are equal. For a ramp, the values are the
  // lower/upper power range at the beginning/end of the segment.
  double start_low = 0.0;
  double start_high = 0.0;
  double end_low = 0.0;
  double end_high = 0.0;
  std::optional<CadenceRange> cadence;
};

struct Workout {
  std::string id;
  std::string name;
  std::string description;
  std::string sport = "cycling";
  std::vector<WorkoutSegment> segments;
  std::string source_hash;
  std::string parser_version = "zwo-v1";
  std::uint32_t source_size = 0U;
  bool has_warnings = false;
  std::vector<std::string> warnings;

  std::uint32_t duration_ms() const {
    std::uint64_t total = 0U;
    for (const auto& segment : segments) total += segment.duration_ms;
    return total > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(total);
  }
};

struct ProfileSettings {
  int ftp_watts = 250;
  double rider_kg = 75.0;
  double bike_kg = 8.0;
  double crr = 0.005;
  double cda = 0.32;
  bool ftp_local_override = false;
  bool weight_local_override = false;
  std::string timezone = "Europe/Berlin";
  std::string timezone_rule = "CET-1CEST,M3.5.0/2,M10.5.0/3";
  std::uint32_t timezone_rule_version = 1U;
};

struct Secrets {
  std::string wifi_ssid;
  std::string wifi_password;
  std::string athlete_id;
  std::string api_key;
  std::string local_admin_secret;
};

struct TrainerCapabilities {
  bool ftms_present = false;
  bool control_point_present = false;
  bool indoor_bike_data_present = false;
  bool feature_present = false;
  bool power_range_present = false;
  int power_min_watts = 0;
  int power_max_watts = 2000;
  int power_increment_watts = 1;
};

enum class LinkState : std::uint8_t {
  kDisconnected,
  kConnecting,
  kConnected,
  kReconnecting,
  kFault,
};

template <typename T>
struct TimedField {
  T value{};
  std::uint64_t timestamp_ms = 0U;
  bool valid = false;

  bool fresh(std::uint64_t now_ms, std::uint32_t max_age_ms) const {
    return valid && now_ms >= timestamp_ms && (now_ms - timestamp_ms) <= max_age_ms;
  }
};

struct SensorSnapshot {
  TimedField<int> power_w;
  TimedField<double> cadence_rpm;
  TimedField<int> heart_rate_bpm;
  TimedField<double> trainer_speed_kph;
  TimedField<std::uint32_t> distance_m;
  LinkState trainer_link = LinkState::kDisconnected;
  LinkState heart_rate_link = LinkState::kDisconnected;
  bool telemetry_subscription = false;
  bool control_granted = false;
};

enum SampleValidity : std::uint16_t {
  kSamplePowerValid = 1U << 0U,
  kSampleCadenceValid = 1U << 1U,
  kSampleHeartRateValid = 1U << 2U,
  kSampleSpeedValid = 1U << 3U,
  kSampleDistanceValid = 1U << 4U,
  kSampleAltitudeValid = 1U << 5U,
  kSampleGradeValid = 1U << 6U,
  kSampleTrainerLink = 1U << 7U,
  kSampleHeartRateLink = 1U << 8U,
  kSamplePowerStale = 1U << 9U,
  kSampleSimulationEnabled = 1U << 10U,
  kSampleInterrupted = 1U << 11U,
};

struct TelemetrySample {
  std::uint32_t sequence = 0U;
  std::uint32_t wall_ms = 0U;
  std::uint32_t active_ms = 0U;
  std::uint32_t plan_ms = 0U;
  std::int16_t power_w = 0;
  std::uint16_t target_w = 0U;
  std::uint16_t cadence_half_rpm = 0U;
  std::uint16_t heart_rate_bpm = 0U;
  std::uint32_t speed_mm_s = 0U;
  std::uint32_t distance_cm = 0U;
  std::int32_t relative_altitude_cm = 0;
  std::int16_t grade_hundredths_percent = 0;
  std::uint16_t validity = 0U;
  std::uint8_t extension[8]{};
};

struct SessionEvent {
  enum class Type : std::uint8_t {
    kStart,
    kPause,
    kResume,
    kStop,
    kTrainerDisconnected,
    kTrainerReconnected,
    kPowerStale,
    kStorageFault,
    kRecovery,
  };

  Type type = Type::kStart;
  std::uint32_t wall_ms = 0U;
  std::uint32_t active_ms = 0U;
  std::uint32_t plan_ms = 0U;
  std::string reason;
};

struct SessionSummary {
  std::string uuid;
  std::string workout_id;
  std::string workout_name;
  bool complete = false;
  bool interrupted = false;
  std::uint32_t wall_ms = 0U;
  std::uint32_t active_ms = 0U;
  std::uint32_t plan_ms = 0U;
  std::uint32_t sample_count = 0U;
  std::uint32_t valid_power_count = 0U;
  std::uint32_t valid_heart_rate_count = 0U;
  std::int32_t average_power_w = 0;
  std::int32_t max_power_w = 0;
  std::int32_t average_cadence_rpm = 0;
  std::int32_t average_heart_rate_bpm = 0;
  std::int32_t max_heart_rate_bpm = 0;
  std::uint32_t distance_cm = 0U;
  std::int32_t ascent_cm = 0;
  std::string fit_path;
  std::string fit_sha256;
};

}  // namespace trainer_loop
