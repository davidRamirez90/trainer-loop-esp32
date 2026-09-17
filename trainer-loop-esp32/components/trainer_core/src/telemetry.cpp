#include "trainer_loop/telemetry.h"

#include <cstring>

namespace trainer_loop {
namespace {

void put_u16(std::uint8_t* out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xffU);
  out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
  for (unsigned i = 0U; i < 4U; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU);
  }
}

void put_i16(std::uint8_t* out, std::int16_t value) {
  put_u16(out, static_cast<std::uint16_t>(value));
}

void put_i32(std::uint8_t* out, std::int32_t value) {
  put_u32(out, static_cast<std::uint32_t>(value));
}

std::uint16_t get_u16(const std::uint8_t* in) {
  return static_cast<std::uint16_t>(in[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[1]) << 8U);
}

std::uint32_t get_u32(const std::uint8_t* in) {
  std::uint32_t value = 0U;
  for (unsigned i = 0U; i < 4U; ++i) {
    value |= static_cast<std::uint32_t>(in[i]) << (8U * i);
  }
  return value;
}

}  // namespace

std::array<std::uint8_t, kTelemetrySampleBytes> serialize_sample(const TelemetrySample& sample) {
  std::array<std::uint8_t, kTelemetrySampleBytes> bytes{};
  std::size_t offset = 0U;
  put_u32(bytes.data() + offset, sample.sequence);
  offset += 4U;
  put_u32(bytes.data() + offset, sample.wall_ms);
  offset += 4U;
  put_u32(bytes.data() + offset, sample.active_ms);
  offset += 4U;
  put_u32(bytes.data() + offset, sample.plan_ms);
  offset += 4U;
  put_i16(bytes.data() + offset, sample.power_w);
  offset += 2U;
  put_u16(bytes.data() + offset, sample.target_w);
  offset += 2U;
  put_u16(bytes.data() + offset, sample.cadence_half_rpm);
  offset += 2U;
  put_u16(bytes.data() + offset, sample.heart_rate_bpm);
  offset += 2U;
  put_u32(bytes.data() + offset, sample.speed_mm_s);
  offset += 4U;
  put_u32(bytes.data() + offset, sample.distance_cm);
  offset += 4U;
  put_i32(bytes.data() + offset, sample.relative_altitude_cm);
  offset += 4U;
  put_i16(bytes.data() + offset, sample.grade_hundredths_percent);
  offset += 2U;
  put_u16(bytes.data() + offset, sample.validity);
  offset += 2U;
  std::memcpy(bytes.data() + offset, sample.extension, sizeof(sample.extension));
  return bytes;
}

Result<TelemetrySample> deserialize_sample(const std::uint8_t* bytes, std::size_t length) {
  if (bytes == nullptr || length < kTelemetrySampleBytes) {
    return Result<TelemetrySample>::failure(
        ErrorCode::kMalformedInput, "telemetry sample is truncated");
  }

  TelemetrySample sample;
  std::size_t offset = 0U;
  sample.sequence = get_u32(bytes + offset);
  offset += 4U;
  sample.wall_ms = get_u32(bytes + offset);
  offset += 4U;
  sample.active_ms = get_u32(bytes + offset);
  offset += 4U;
  sample.plan_ms = get_u32(bytes + offset);
  offset += 4U;
  sample.power_w = static_cast<std::int16_t>(get_u16(bytes + offset));
  offset += 2U;
  sample.target_w = get_u16(bytes + offset);
  offset += 2U;
  sample.cadence_half_rpm = get_u16(bytes + offset);
  offset += 2U;
  sample.heart_rate_bpm = get_u16(bytes + offset);
  offset += 2U;
  sample.speed_mm_s = get_u32(bytes + offset);
  offset += 4U;
  sample.distance_cm = get_u32(bytes + offset);
  offset += 4U;
  sample.relative_altitude_cm = static_cast<std::int32_t>(get_u32(bytes + offset));
  offset += 4U;
  sample.grade_hundredths_percent = static_cast<std::int16_t>(get_u16(bytes + offset));
  offset += 2U;
  sample.validity = get_u16(bytes + offset);
  offset += 2U;
  std::memcpy(sample.extension, bytes + offset, sizeof(sample.extension));
  return Result<TelemetrySample>::success(sample);
}

std::uint32_t crc32(const std::uint8_t* bytes, std::size_t length) {
  std::uint32_t crc = 0xffffffffU;
  if (bytes == nullptr && length != 0U) return 0U;
  for (std::size_t i = 0U; i < length; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

std::uint32_t crc32(const std::vector<std::uint8_t>& bytes) {
  return crc32(bytes.data(), bytes.size());
}

}  // namespace trainer_loop
