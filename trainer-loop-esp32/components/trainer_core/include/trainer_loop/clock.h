#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <cstdint>
#include <optional>

namespace trainer_loop {

std::uint64_t monotonic_now_ms();

struct ClockSnapshot {
  std::uint32_t wall_ms = 0U;
  std::uint32_t active_ms = 0U;
  std::uint32_t plan_ms = 0U;
  bool running = false;
  bool paused = false;
};

struct StartTimeAnchor {
  std::int64_t utc_ms = 0;
  bool trusted = false;
  std::uint8_t quality = 0U;
};

class SessionClocks {
 public:
  Status start(std::uint64_t monotonic_ms, StartTimeAnchor anchor);
  Status pause(std::uint64_t monotonic_ms);
  Status resume(std::uint64_t monotonic_ms);
  Status finish(std::uint64_t monotonic_ms);

  ClockSnapshot snapshot(std::uint64_t monotonic_ms) const;
  std::optional<std::int64_t> utc_at(std::uint64_t monotonic_ms) const;
  const StartTimeAnchor& anchor() const { return anchor_; }
  bool started() const { return started_; }
  bool running() const { return started_ && running_; }

 private:
  std::uint64_t bounded_now(std::uint64_t monotonic_ms) const;
  std::uint32_t saturating_u32(std::uint64_t value) const;

  bool started_ = false;
  bool running_ = false;
  bool finished_ = false;
  std::uint64_t start_monotonic_ms_ = 0U;
  std::uint64_t last_transition_ms_ = 0U;
  std::uint64_t active_accumulated_ms_ = 0U;
  mutable std::uint64_t last_observed_ms_ = 0U;
  StartTimeAnchor anchor_;
};

}  // namespace trainer_loop
