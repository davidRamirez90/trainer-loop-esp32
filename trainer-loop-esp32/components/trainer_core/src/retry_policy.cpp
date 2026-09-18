#include "trainer_loop/retry_policy.h"

#include <array>
#include <limits>

namespace trainer_loop {

namespace {

constexpr std::array<std::uint64_t, 5> kRetryDelaysMs = {
    30'000ULL, 120'000ULL, 600'000ULL, 1'800'000ULL, 7'200'000ULL};

}  // namespace

void RetryPolicy::request(std::uint64_t now_ms) {
  request_after(now_ms, 0U);
}

void RetryPolicy::request_after(std::uint64_t now_ms, std::uint64_t delay_ms) {
  pending_ = true;
  next_due_ms_ = now_ms > std::numeric_limits<std::uint64_t>::max() - delay_ms
      ? std::numeric_limits<std::uint64_t>::max() : now_ms + delay_ms;
  failures_ = 0U;
}

bool RetryPolicy::due(std::uint64_t now_ms, bool ride_active, bool busy) const {
  return pending_ && !ride_active && !busy && now_ms >= next_due_ms_;
}

void RetryPolicy::begin() {
  pending_ = false;
}

void RetryPolicy::finish(std::uint64_t now_ms, bool success) {
  if (success) {
    pending_ = false;
    failures_ = 0U;
    return;
  }
  const std::size_t delay_index = failures_ < kRetryDelaysMs.size()
      ? static_cast<std::size_t>(failures_) : kRetryDelaysMs.size() - 1U;
  const std::uint64_t delay = kRetryDelaysMs[delay_index];
  next_due_ms_ = now_ms > std::numeric_limits<std::uint64_t>::max() - delay
      ? std::numeric_limits<std::uint64_t>::max() : now_ms + delay;
  pending_ = true;
  if (failures_ < kRetryDelaysMs.size()) ++failures_;
}

}  // namespace trainer_loop
