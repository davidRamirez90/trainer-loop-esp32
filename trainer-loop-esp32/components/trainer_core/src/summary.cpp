#include "trainer_loop/summary.h"

#include <algorithm>

namespace trainer_loop {

SessionSummary summarize_session(const std::string& uuid, const Workout& workout,
                                 const std::vector<TelemetrySample>& samples,
                                 std::uint32_t wall_ms, std::uint32_t active_ms,
                                 bool complete, bool interrupted) {
  SessionSummary summary;
  summary.uuid = uuid;
  summary.workout_id = workout.id;
  summary.workout_name = workout.name;
  summary.complete = complete;
  summary.interrupted = interrupted;
  summary.wall_ms = wall_ms;
  summary.active_ms = active_ms;
  summary.plan_ms = samples.empty() ? 0U : samples.back().plan_ms;
  summary.sample_count = static_cast<std::uint32_t>(samples.size());
  std::int64_t power_sum = 0;
  std::int64_t cadence_sum = 0;
  std::int64_t hr_sum = 0;
  std::int32_t last_altitude = 0;
  bool altitude_seen = false;
  for (const auto& sample : samples) {
    if ((sample.validity & kSamplePowerValid) != 0U && sample.power_w >= 0) {
      ++summary.valid_power_count;
      power_sum += sample.power_w;
      summary.max_power_w = std::max(summary.max_power_w, static_cast<std::int32_t>(sample.power_w));
    }
    if ((sample.validity & kSampleCadenceValid) != 0U) {
      const std::int32_t cadence = static_cast<std::int32_t>(sample.cadence_half_rpm / 2U);
      cadence_sum += cadence;
    }
    if ((sample.validity & kSampleHeartRateValid) != 0U && sample.heart_rate_bpm <= 255U) {
      ++summary.valid_heart_rate_count;
      hr_sum += sample.heart_rate_bpm;
      summary.max_heart_rate_bpm = std::max(summary.max_heart_rate_bpm, static_cast<std::int32_t>(sample.heart_rate_bpm));
    }
    if ((sample.validity & kSampleDistanceValid) != 0U) summary.distance_cm = std::max(summary.distance_cm, sample.distance_cm);
    if ((sample.validity & kSampleAltitudeValid) != 0U) {
      const std::int32_t altitude = sample.relative_altitude_cm;
      if (altitude_seen && altitude > last_altitude) summary.ascent_cm += altitude - last_altitude;
      last_altitude = altitude;
      altitude_seen = true;
    }
  }
  if (summary.valid_power_count > 0U) summary.average_power_w = static_cast<std::int32_t>(power_sum / summary.valid_power_count);
  std::uint32_t cadence_count = 0U;
  for (const auto& sample : samples) if ((sample.validity & kSampleCadenceValid) != 0U) ++cadence_count;
  if (cadence_count > 0U) summary.average_cadence_rpm = static_cast<std::int32_t>(cadence_sum / cadence_count);
  if (summary.valid_heart_rate_count > 0U) summary.average_heart_rate_bpm = static_cast<std::int32_t>(hr_sum / summary.valid_heart_rate_count);
  return summary;
}

}  // namespace trainer_loop
