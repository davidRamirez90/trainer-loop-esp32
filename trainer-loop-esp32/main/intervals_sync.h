#pragma once

#include "intervals_http.h"
#include "trainer_loop/sync.h"
#include "trainer_loop/time_service.h"
#include "trainer_loop/workout_cache.h"
#include "trainer_loop/settings.h"

namespace trainer_loop {

class IntervalsSync {
 public:
  IntervalsSync(IntervalsHttpClient* http, WorkoutCache* cache, SettingsStore* settings)
      : http_(http), cache_(cache), settings_(settings) {}

  Result<SyncMetadata> sync_today(const TimeService& time);

 private:
  [[maybe_unused]] IntervalsHttpClient* http_ = nullptr;
  [[maybe_unused]] WorkoutCache* cache_ = nullptr;
  [[maybe_unused]] SettingsStore* settings_ = nullptr;
};

}  // namespace trainer_loop
