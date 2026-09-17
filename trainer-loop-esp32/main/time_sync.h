#pragma once

#include "trainer_loop/time_service.h"

#include <cstdint>

namespace trainer_loop {

class TimeSynchronizer {
 public:
  Status synchronize(TimeService* time, std::uint32_t timeout_ms);

 private:
  [[maybe_unused]] bool initialized_ = false;
};

}  // namespace trainer_loop
