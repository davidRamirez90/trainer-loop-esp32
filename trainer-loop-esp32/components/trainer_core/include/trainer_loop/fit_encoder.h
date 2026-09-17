#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trainer_loop {

struct FitIdentity {
  // 255 is the FIT Development manufacturer value. It is intentionally not
  // Garmin's manufacturer ID; a production product ID is selected only after
  // the device identity is registered in the adopted FIT profile.
  std::uint16_t manufacturer_id = 255U;
  std::uint16_t product_id = 0U;
  std::uint32_t serial_number = 0U;
  std::uint16_t software_version = 1U;
  std::uint8_t hardware_version = 1U;
};

struct FitInput {
  std::int64_t start_utc_ms = 0;
  bool time_trusted = false;
  std::uint32_t wall_duration_ms = 0U;
  std::uint32_t active_duration_ms = 0U;
  std::vector<TelemetrySample> samples;
  std::vector<SessionEvent> events;
  FitIdentity identity;
};

Result<std::vector<std::uint8_t>> encode_fit(const FitInput& input);
bool validate_fit_bytes(const std::vector<std::uint8_t>& bytes);

}  // namespace trainer_loop
