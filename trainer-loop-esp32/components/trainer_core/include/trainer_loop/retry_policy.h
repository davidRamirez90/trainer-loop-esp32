#pragma once

#include <cstdint>

namespace trainer_loop {

// Schedules network sync attempts outside an active ride. A manual request
// resets the delay and is therefore always tried immediately.
class RetryPolicy {
 public:
  void request(std::uint64_t now_ms);
  void request_after(std::uint64_t now_ms, std::uint64_t delay_ms);
  bool due(std::uint64_t now_ms, bool ride_active, bool busy) const;
  void begin();
  void finish(std::uint64_t now_ms, bool success);

  bool pending() const { return pending_; }
  std::uint32_t failures() const { return failures_; }

 private:
  bool pending_ = false;
  std::uint64_t next_due_ms_ = 0U;
  std::uint32_t failures_ = 0U;
};

}  // namespace trainer_loop
