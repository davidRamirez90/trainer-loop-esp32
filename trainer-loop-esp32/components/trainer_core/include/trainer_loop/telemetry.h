#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace trainer_loop {

std::array<std::uint8_t, kTelemetrySampleBytes> serialize_sample(const TelemetrySample& sample);
Result<TelemetrySample> deserialize_sample(const std::uint8_t* bytes, std::size_t length);

inline Result<TelemetrySample> deserialize_sample(
    const std::array<std::uint8_t, kTelemetrySampleBytes>& bytes) {
  return deserialize_sample(bytes.data(), bytes.size());
}

std::uint32_t crc32(const std::uint8_t* bytes, std::size_t length);
std::uint32_t crc32(const std::vector<std::uint8_t>& bytes);

}  // namespace trainer_loop
