#include "trainer_loop/clock.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <chrono>
#endif

#include <limits>

namespace trainer_loop {

namespace {

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
  return left > std::numeric_limits<std::uint64_t>::max() - right
             ? std::numeric_limits<std::uint64_t>::max()
             : left + right;
}

}  // namespace

std::uint64_t monotonic_now_ms() {
#ifdef ESP_PLATFORM
  return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
#else
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
#endif
}

std::uint64_t SessionClocks::bounded_now(std::uint64_t monotonic_ms) const {
  if (monotonic_ms > last_observed_ms_) last_observed_ms_ = monotonic_ms;
  return last_observed_ms_;
}

std::uint32_t SessionClocks::saturating_u32(std::uint64_t value) const {
  return value > std::numeric_limits<std::uint32_t>::max()
             ? std::numeric_limits<std::uint32_t>::max()
             : static_cast<std::uint32_t>(value);
}

Status SessionClocks::start(std::uint64_t monotonic_ms, StartTimeAnchor anchor) {
  if (started_) return Status::failure(ErrorCode::kConflict, "session clock already started");
  started_ = true;
  running_ = true;
  start_monotonic_ms_ = monotonic_ms;
  last_transition_ms_ = monotonic_ms;
  last_observed_ms_ = monotonic_ms;
  active_accumulated_ms_ = 0U;
  anchor_ = anchor;
  return Status::ok();
}

Status SessionClocks::pause(std::uint64_t monotonic_ms) {
  if (!started_ || !running_) {
    return Status::failure(ErrorCode::kConflict, "session clock is not running");
  }
  const std::uint64_t now = bounded_now(monotonic_ms);
  active_accumulated_ms_ = saturating_add(active_accumulated_ms_, now - last_transition_ms_);
  last_transition_ms_ = now;
  last_observed_ms_ = now;
  running_ = false;
  return Status::ok();
}

Status SessionClocks::resume(std::uint64_t monotonic_ms) {
  if (!started_ || running_) {
    return Status::failure(ErrorCode::kConflict, "session clock is not paused");
  }
  const std::uint64_t now = bounded_now(monotonic_ms);
  last_transition_ms_ = now;
  last_observed_ms_ = now;
  running_ = true;
  return Status::ok();
}

Status SessionClocks::finish(std::uint64_t monotonic_ms) {
  if (!started_) return Status::failure(ErrorCode::kConflict, "session clock is not started");
  if (running_) {
    const std::uint64_t now = bounded_now(monotonic_ms);
    active_accumulated_ms_ = saturating_add(active_accumulated_ms_, now - last_transition_ms_);
    last_transition_ms_ = now;
    last_observed_ms_ = now;
    running_ = false;
  } else {
    last_observed_ms_ = bounded_now(monotonic_ms);
  }
  return Status::ok();
}

ClockSnapshot SessionClocks::snapshot(std::uint64_t monotonic_ms) const {
  if (!started_) return ClockSnapshot{};
  const std::uint64_t now = bounded_now(monotonic_ms);
  const std::uint64_t wall = now - start_monotonic_ms_;
  const std::uint64_t active = saturating_add(active_accumulated_ms_,
                                              running_ ? (now - last_transition_ms_) : 0U);
  const std::uint32_t active_u32 = saturating_u32(active);
  return ClockSnapshot{saturating_u32(wall), active_u32, active_u32, running_, !running_};
}

std::optional<std::int64_t> SessionClocks::utc_at(std::uint64_t monotonic_ms) const {
  if (!started_ || !anchor_.trusted) return std::nullopt;
  const std::uint64_t wall = bounded_now(monotonic_ms) - start_monotonic_ms_;
  const std::int64_t wall_signed = wall > static_cast<std::uint64_t>(INT64_MAX)
                                        ? INT64_MAX
                                        : static_cast<std::int64_t>(wall);
  if (anchor_.utc_ms > INT64_MAX - wall_signed) return std::nullopt;
  return anchor_.utc_ms + wall_signed;
}

}  // namespace trainer_loop
