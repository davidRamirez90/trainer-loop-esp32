#include "trainer_loop/time_service.h"

#include <cstdlib>
#include <ctime>
#include <limits>

namespace trainer_loop {
namespace {

constexpr std::int64_t kMinimumPlausibleUtcMs = 1577836800000LL;  // 2020-01-01
constexpr std::int64_t kMaximumPlausibleUtcMs = 4102444800000LL;  // 2100-01-01

bool leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
  switch (month) {
    case 2:
      return leap_year(year) ? 29 : 28;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    default:
      return 31;
  }
}

// Howard Hinnant's civil-date conversion, kept local so the ESP build does
// not depend on a libc timezone or calendar extension.
std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2U ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned adjusted_month = month > 2U ? month - 3U : month + 9U;
  const unsigned day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
  const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
  return static_cast<std::int64_t>(era) * 146097LL + static_cast<std::int64_t>(day_of_era) - 719468LL;
}

}  // namespace

bool plausible_utc_ms(std::int64_t utc_ms) {
  return utc_ms >= kMinimumPlausibleUtcMs && utc_ms < kMaximumPlausibleUtcMs;
}

Result<std::int64_t> utc_ms_from_calendar(const CalendarDateTime& date_time) {
  if (date_time.year < 1970 || date_time.year >= 2100 || date_time.month < 1 || date_time.month > 12 ||
      date_time.day < 1 || date_time.day > days_in_month(date_time.year, date_time.month) ||
      date_time.hour < 0 || date_time.hour > 23 || date_time.minute < 0 || date_time.minute > 59 ||
      date_time.second < 0 || date_time.second > 59) {
    return Result<std::int64_t>::failure(ErrorCode::kTimeUntrusted, "calendar date/time is invalid");
  }
  const std::int64_t days = days_from_civil(date_time.year, static_cast<unsigned>(date_time.month),
                                            static_cast<unsigned>(date_time.day));
  const std::int64_t seconds = days * 86400LL + static_cast<std::int64_t>(date_time.hour) * 3600LL +
                               static_cast<std::int64_t>(date_time.minute) * 60LL + date_time.second;
  if (seconds > std::numeric_limits<std::int64_t>::max() / 1000LL ||
      seconds < std::numeric_limits<std::int64_t>::min() / 1000LL) {
    return Result<std::int64_t>::failure(ErrorCode::kOverflow, "calendar date/time overflows UTC milliseconds");
  }
  return Result<std::int64_t>::success(seconds * 1000LL);
}

std::string local_date_for_utc(std::int64_t utc_ms, const std::string& posix_rule) {
  if (!plausible_utc_ms(utc_ms) || posix_rule.empty() || posix_rule.size() > 96U) return {};
  const std::time_t seconds = static_cast<std::time_t>(utc_ms / 1000LL);
  const char* old_tz = std::getenv("TZ");
  const std::string old_value = old_tz == nullptr ? std::string{} : std::string(old_tz);
  const bool had_old = old_tz != nullptr;
  ::setenv("TZ", posix_rule.c_str(), 1);
  ::tzset();
  std::tm local_time{};
  const bool converted = localtime_r(&seconds, &local_time) != nullptr;
  if (had_old) ::setenv("TZ", old_value.c_str(), 1);
  else ::unsetenv("TZ");
  ::tzset();
  if (!converted) return {};
  char buffer[11]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local_time) == 0U) return {};
  return buffer;
}

TimeSnapshot TimeService::snapshot() const {
  TimeSnapshot result = snapshot_;
  if (result.quality == TimeQuality::kInvalid) return result;
  const auto now = now_();
  if (now > observed_monotonic_ms_) observed_monotonic_ms_ = now;
  const auto elapsed = observed_monotonic_ms_ - anchor_monotonic_ms_;
  if (elapsed > static_cast<std::uint64_t>(INT64_MAX - result.utc_ms)) return TimeSnapshot{};
  result.utc_ms += static_cast<std::int64_t>(elapsed);
  return result;
}

Status TimeService::set(std::int64_t utc_ms, TimeQuality quality, bool trusted) {
  if (!plausible_utc_ms(utc_ms)) return Status::failure(ErrorCode::kTimeUntrusted, "UTC is outside plausible bounds");
  if (snapshot_.trusted && snapshot_.utc_ms != utc_ms && snapshot_.correction_count < UINT32_MAX) {
    ++snapshot_.correction_count;
  }
  anchor_monotonic_ms_ = observed_monotonic_ms_ = now_();
  snapshot_.utc_ms = utc_ms;
  snapshot_.quality = quality;
  snapshot_.trusted = trusted;
  return Status::ok();
}

Status TimeService::set_from_rtc(std::int64_t utc_ms, bool rtc_power_valid) {
  return set(utc_ms, TimeQuality::kRtc, rtc_power_valid);
}

Status TimeService::set_from_sntp(std::int64_t utc_ms) {
  return set(utc_ms, TimeQuality::kSntp, true);
}

Status TimeService::set_manual(std::int64_t utc_ms) {
  return set(utc_ms, TimeQuality::kManual, true);
}

void TimeService::invalidate() {
  snapshot_ = TimeSnapshot{};
}

}  // namespace trainer_loop
