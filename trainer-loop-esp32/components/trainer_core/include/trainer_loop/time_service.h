#pragma once

#include "trainer_loop/result.h"

#include "trainer_loop/clock.h"
#include <functional>
#include <utility>
#include <cstdint>
#include <string>

namespace trainer_loop {

enum class TimeQuality : std::uint8_t { kInvalid, kManual, kRtc, kSntp };

struct TimeSnapshot {
  std::int64_t utc_ms = 0;
  TimeQuality quality = TimeQuality::kInvalid;
  bool trusted = false;
  std::uint32_t correction_count = 0U;
};

struct CalendarDateTime {
  int year = 1970;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

bool plausible_utc_ms(std::int64_t utc_ms);
Result<std::int64_t> utc_ms_from_calendar(const CalendarDateTime& date_time);
std::string local_date_for_utc(std::int64_t utc_ms, const std::string& posix_rule);

class TimeService {
 public:
  explicit TimeService(std::function<std::uint64_t()> now = monotonic_now_ms) : now_(std::move(now)) {}
  Status set_from_rtc(std::int64_t utc_ms, bool rtc_power_valid);
  Status set_from_sntp(std::int64_t utc_ms);
  Status set_manual(std::int64_t utc_ms);
  void invalidate();

  TimeSnapshot snapshot() const;
  bool trusted() const { return snapshot_.trusted; }

 private:
  Status set(std::int64_t utc_ms, TimeQuality quality, bool trusted);
  TimeSnapshot snapshot_;
  std::function<std::uint64_t()> now_;
  std::uint64_t anchor_monotonic_ms_ = 0;
  mutable std::uint64_t observed_monotonic_ms_ = 0;
};

}  // namespace trainer_loop
