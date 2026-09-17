#pragma once

#include "trainer_loop/models.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trainer_loop {

SessionSummary summarize_session(const std::string& uuid, const Workout& workout,
                                 const std::vector<TelemetrySample>& samples,
                                 std::uint32_t wall_ms, std::uint32_t active_ms,
                                 bool complete, bool interrupted);

}  // namespace trainer_loop
