#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trainer_loop {

struct CachedWorkout {
  std::string athlete_id;
  std::string event_id;
  std::string local_date;
  std::string timezone;
  std::string name;
  std::string source_zwo;
  Workout workout;
  int ftp_used_for_preview = 0;
  std::int64_t downloaded_utc_ms = 0;
  std::int64_t validated_utc_ms = 0;
};

class WorkoutCache {
 public:
  explicit WorkoutCache(std::string directory);

  Status initialize();
  Status save(const CachedWorkout& cached);
  Result<CachedWorkout> load(const std::string& event_id) const;
  std::vector<CachedWorkout> list_for_date(const std::string& local_date) const;

 private:
  std::string path_for(const std::string& event_id) const;
  std::string directory_;
};

}  // namespace trainer_loop
