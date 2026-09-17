#pragma once

#include "trainer_loop/ble_protocol.h"
#include "trainer_loop/result.h"

#include <cstdint>
#include <optional>

namespace trainer_loop {

struct TargetCommand {
  int watts = 0;
  std::uint64_t requested_at_ms = 0U;
  bool safety = false;
};

class ErgTargetScheduler {
 public:
  Status offer(int watts, std::uint64_t now_ms);
  Status take_due(std::uint64_t now_ms, TargetCommand* command);
  void mark_sent(const TargetCommand& command, std::uint64_t now_ms);
  void preempt_for_safety();
  void reset();

  bool has_pending() const { return pending_.has_value(); }
  int last_sent_watts() const { return last_sent_watts_; }

 private:
  std::optional<TargetCommand> pending_;
  int last_sent_watts_ = -1;
  std::uint64_t last_sent_at_ms_ = 0U;
  bool has_sent_ = false;
  bool safety_preempted_ = false;
};

class ErgPowerNegotiator {
 public:
  explicit ErgPowerNegotiator(TrainerCapabilities capabilities) : capabilities_(capabilities) {}
  Result<int> validate_target(int watts) const;
  const TrainerCapabilities& capabilities() const { return capabilities_; }

 private:
  TrainerCapabilities capabilities_;
};

}  // namespace trainer_loop
