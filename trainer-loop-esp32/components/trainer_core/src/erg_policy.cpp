#include "trainer_loop/erg_policy.h"

#include <algorithm>

namespace trainer_loop {

namespace {

bool before_deadline(std::uint64_t now_ms, std::uint64_t start_ms, std::uint64_t duration_ms) {
  return now_ms < (start_ms > UINT64_MAX - duration_ms ? UINT64_MAX : start_ms + duration_ms);
}

}  // namespace

Status ErgTargetScheduler::offer(int watts, std::uint64_t now_ms) {
  if (watts < 0 || watts > 2000) return Status::failure(ErrorCode::kBounds, "ERG target is outside product bounds");
  if (safety_preempted_) return Status::failure(ErrorCode::kConflict, "ERG target queue is safety-preempted");
  pending_ = TargetCommand{watts, now_ms, false};
  return Status::ok();
}

Status ErgTargetScheduler::take_due(std::uint64_t now_ms, TargetCommand* command) {
  if (command == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "ERG command output is null");
  if (safety_preempted_) return Status::failure(ErrorCode::kConflict, "ERG target queue is safety-preempted");
  if (!pending_.has_value()) return Status::failure(ErrorCode::kConflict, "no ERG target is pending");
  if (has_sent_ && before_deadline(now_ms, last_sent_at_ms_, 1000U)) return Status::failure(ErrorCode::kConflict, "ERG target rate limit");
  if (has_sent_ && pending_->watts == last_sent_watts_ && before_deadline(now_ms, last_sent_at_ms_, 5000U)) {
    pending_.reset();
    return Status::failure(ErrorCode::kConflict, "unchanged ERG target is suppressed");
  }
  *command = *pending_;
  return Status::ok();
}

void ErgTargetScheduler::mark_sent(const TargetCommand& command, std::uint64_t now_ms) {
  last_sent_watts_ = command.watts;
  last_sent_at_ms_ = now_ms;
  has_sent_ = true;
  if (pending_.has_value() && pending_->watts == command.watts) pending_.reset();
}

void ErgTargetScheduler::preempt_for_safety() {
  pending_.reset();
  safety_preempted_ = true;
}

void ErgTargetScheduler::reset() {
  pending_.reset();
  last_sent_watts_ = -1;
  last_sent_at_ms_ = 0U;
  has_sent_ = false;
  safety_preempted_ = false;
}

Result<int> ErgPowerNegotiator::validate_target(int watts) const {
  const auto encoded = encode_target_power(watts, capabilities_);
  if (!encoded) return Result<int>::failure(encoded.error->code, encoded.error->message);
  const int quantized = static_cast<int>(encoded.value->at(1)) |
                        (static_cast<int>(encoded.value->at(2)) << 8);
  return Result<int>::success(quantized);
}

}  // namespace trainer_loop
