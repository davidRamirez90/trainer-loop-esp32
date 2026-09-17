#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace trainer_loop {

struct WorkoutParseLimits {
  std::size_t max_source_bytes = kMaxWorkoutSourceBytes;
  std::size_t max_segments = kMaxWorkoutSegments;
  std::uint32_t max_duration_ms = kMaxWorkoutDurationMs;
};

struct TargetRange {
  double low = 0.0;
  double high = 0.0;
};

Result<Workout> parse_zwo(const std::string& source,
                          const WorkoutParseLimits& limits = WorkoutParseLimits{});
Status validate_workout(const Workout& workout, int ftp_watts);

TargetRange target_range_at(const Workout& workout, std::uint32_t plan_ms);
TargetRange resolved_target_range_at(const Workout& workout,
                                     std::uint32_t plan_ms,
                                     int ftp_watts);
Result<std::vector<int>> resolve_segment_targets(const Workout& workout, int ftp_watts);

}  // namespace trainer_loop
