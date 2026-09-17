#include "trainer_loop/fit_encoder.h"

#include "trainer_loop/limits.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace trainer_loop {
namespace {

constexpr std::uint32_t kFitEpochOffsetSeconds = 631065600U;
constexpr std::uint8_t kFitHeaderSize = 14U;
constexpr std::uint8_t kProtocolVersion = 0x10U;
constexpr std::uint16_t kProfileVersion = 0x0100U;
constexpr std::uint8_t kInvalidU8 = 0xffU;
constexpr std::uint16_t kInvalidU16 = 0xffffU;
constexpr std::uint32_t kInvalidU32 = 0xffffffffU;
constexpr std::size_t kMaxFitEvents = 256U;

enum BaseType : std::uint8_t {
  kEnum = 0x00U,
  kUint8 = 0x02U,
  kUint16 = 0x84U,
  kUint32 = 0x86U,
  kUint32z = 0x8cU,
};

struct Field {
  std::uint8_t number = 0U;
  std::uint8_t size = 0U;
  BaseType base_type = kUint8;
};

using Value = std::optional<std::int64_t>;

void put_u16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void put_u32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  for (unsigned i = 0U; i < 4U; ++i) bytes->push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
}

std::uint16_t get_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0U;
  for (unsigned i = 0U; i < 4U; ++i) value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8U * i);
  return value;
}

void put_value(std::vector<std::uint8_t>* bytes, const Field& field, Value value) {
  std::uint64_t encoded = 0U;
  if (!value.has_value()) {
    if (field.base_type == kUint8 || field.base_type == kEnum) encoded = kInvalidU8;
    else if (field.base_type == kUint16) encoded = kInvalidU16;
    else if (field.base_type == kUint32z) encoded = 0U;
    else encoded = kInvalidU32;
  } else {
    const std::int64_t raw = *value;
    encoded = raw < 0 ? 0U : static_cast<std::uint64_t>(raw);
    const std::uint64_t maximum = field.base_type == kUint8 || field.base_type == kEnum
                                      ? kInvalidU8
                                      : (field.base_type == kUint16 ? kInvalidU16 : kInvalidU32);
    encoded = std::min(encoded, maximum);
  }
  for (std::uint8_t i = 0U; i < field.size; ++i) {
    bytes->push_back(static_cast<std::uint8_t>((encoded >> (8U * i)) & 0xffU));
  }
}

void definition(std::vector<std::uint8_t>* bytes, std::uint8_t local,
                std::uint16_t global, const std::vector<Field>& fields) {
  bytes->push_back(static_cast<std::uint8_t>(0x40U | (local & 0x0fU)));
  bytes->push_back(0U);  // reserved
  bytes->push_back(0U);  // little endian
  put_u16(bytes, global);
  bytes->push_back(static_cast<std::uint8_t>(fields.size()));
  for (const auto& field : fields) {
    bytes->push_back(field.number);
    bytes->push_back(field.size);
    bytes->push_back(static_cast<std::uint8_t>(field.base_type));
  }
}

void data(std::vector<std::uint8_t>* bytes, std::uint8_t local,
          const std::vector<Field>& fields, const std::vector<Value>& values) {
  bytes->push_back(static_cast<std::uint8_t>(local & 0x0fU));
  for (std::size_t i = 0U; i < fields.size(); ++i) put_value(bytes, fields[i], values[i]);
}

constexpr std::uint16_t kCrcTable[16] = {
    0x0000U, 0xcc01U, 0xd801U, 0x1400U, 0xf001U, 0x3c00U, 0x2800U, 0xe401U,
    0xa001U, 0x6c00U, 0x7800U, 0xb401U, 0x5000U, 0x9c01U, 0x8801U, 0x4400U};

std::uint16_t fit_crc(const std::uint8_t* bytes, std::size_t length) {
  std::uint16_t crc = 0U;
  for (std::size_t i = 0U; i < length; ++i) {
    const std::uint8_t value = bytes[i];
    std::uint16_t temporary = kCrcTable[crc & 0x0fU];
    crc = static_cast<std::uint16_t>((crc >> 4U) ^ temporary ^ kCrcTable[value & 0x0fU]);
    temporary = kCrcTable[crc & 0x0fU];
    crc = static_cast<std::uint16_t>((crc >> 4U) ^ temporary ^ kCrcTable[(value >> 4U) & 0x0fU]);
  }
  return crc;
}

std::optional<std::uint32_t> fit_timestamp(std::int64_t utc_ms) {
  if (utc_ms < static_cast<std::int64_t>(kFitEpochOffsetSeconds) * 1000LL) return std::nullopt;
  const std::int64_t seconds = utc_ms / 1000LL - static_cast<std::int64_t>(kFitEpochOffsetSeconds);
  if (seconds < 0 || seconds > static_cast<std::int64_t>(kInvalidU32)) return std::nullopt;
  return static_cast<std::uint32_t>(seconds);
}

std::vector<TelemetrySample> normalize_samples(const std::vector<TelemetrySample>& samples) {
  std::vector<TelemetrySample> normalized;
  normalized.reserve(samples.size());
  for (const auto& sample : samples) {
    if (!normalized.empty() && sample.wall_ms == normalized.back().wall_ms) {
      normalized.back() = sample;
    } else if (normalized.empty() || sample.wall_ms > normalized.back().wall_ms) {
      normalized.push_back(sample);
    }
  }
  return normalized;
}

template <typename Selector>
Value average(const std::vector<TelemetrySample>& samples, std::uint16_t bit,
                     const Selector& selector) {
  std::int64_t sum = 0;
  std::uint32_t count = 0U;
  for (const auto& sample : samples) {
    if ((sample.validity & bit) == 0U) continue;
    // Negative trainer power is a valid internal control value in some
    // protocols, but FIT power is unsigned. Do not let it contaminate the
    // session statistics when the corresponding Record field is omitted.
    if (bit == kSamplePowerValid && sample.power_w < 0) continue;
    const std::int64_t value = selector(sample);
    if ((bit == kSampleCadenceValid || bit == kSampleHeartRateValid) && (value < 0 || value >= 255)) continue;
    sum += value;
    ++count;
  }
  return count == 0U ? std::nullopt : Value{sum / static_cast<std::int64_t>(count)};
}

template <typename Selector>
Value maximum(const std::vector<TelemetrySample>& samples, std::uint16_t bit,
                     const Selector& selector) {
  std::int64_t result = 0;
  bool found = false;
  for (const auto& sample : samples) {
    if ((sample.validity & bit) == 0U) continue;
    if (bit == kSamplePowerValid && sample.power_w < 0) continue;
    const std::int64_t value = selector(sample);
    if ((bit == kSampleCadenceValid || bit == kSampleHeartRateValid) && (value < 0 || value >= 255)) continue;
    result = found ? std::max(result, value) : value;
    found = true;
  }
  return found ? Value{result} : std::nullopt;
}

std::vector<SessionEvent> normalized_events(const FitInput& input, std::uint32_t wall_ms) {
  std::vector<SessionEvent> events;
  events.push_back(SessionEvent{SessionEvent::Type::kStart, 0U, 0U, 0U, "start"});
  for (const auto& event : input.events) {
    if (event.type == SessionEvent::Type::kPause || event.type == SessionEvent::Type::kResume ||
        event.type == SessionEvent::Type::kTrainerDisconnected) {
      auto timer_event = event;
      if (timer_event.type == SessionEvent::Type::kTrainerDisconnected) timer_event.type = SessionEvent::Type::kPause;
      events.push_back(timer_event);
    }
  }
  events.push_back(SessionEvent{SessionEvent::Type::kStop, wall_ms, input.active_duration_ms,
                                input.active_duration_ms, "stop"});
  std::stable_sort(events.begin(), events.end(), [](const SessionEvent& left, const SessionEvent& right) {
    return left.wall_ms < right.wall_ms;
  });
  return events;
}

}  // namespace

Result<std::vector<std::uint8_t>> encode_fit(const FitInput& input) {
  if (input.samples.size() > kMaxSessionSamples || input.events.size() > kMaxFitEvents) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kBounds, "FIT input exceeds bounded session limits");
  }
  if (!input.time_trusted) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kTimeUntrusted, "FIT requires a trusted UTC anchor");
  const auto start_timestamp = fit_timestamp(input.start_utc_ms);
  if (!start_timestamp) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kTimeUntrusted, "FIT start is outside representable epoch");
  for (std::size_t index = 1U; index < input.samples.size(); ++index) {
    if (input.samples[index].wall_ms < input.samples[index - 1U].wall_ms) {
      return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kInvalidArgument, "FIT samples are not time ordered");
    }
  }
  const auto samples = normalize_samples(input.samples);
  const std::uint32_t wall_ms = std::max(input.wall_duration_ms, samples.empty() ? 0U : samples.back().wall_ms);
  const std::uint32_t active_ms = input.active_duration_ms;
  if (active_ms > wall_ms) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kInvalidArgument, "FIT active time exceeds elapsed time");
  if (input.start_utc_ms > INT64_MAX - static_cast<std::int64_t>(wall_ms)) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kTimeUntrusted, "FIT end time overflows UTC");
  }
  const auto end_timestamp = fit_timestamp(input.start_utc_ms + static_cast<std::int64_t>(wall_ms));
  if (!end_timestamp) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kTimeUntrusted, "FIT end is outside representable epoch");

  double minimum_altitude = 0.0;
  double maximum_altitude = 0.0;
  bool found_altitude = false;
  for (const auto& sample : samples) {
    if ((sample.validity & kSampleAltitudeValid) == 0U) continue;
    const double altitude = static_cast<double>(sample.relative_altitude_cm) / 100.0;
    minimum_altitude = found_altitude ? std::min(minimum_altitude, altitude) : altitude;
    maximum_altitude = found_altitude ? std::max(maximum_altitude, altitude) : altitude;
    found_altitude = true;
  }
  if (found_altitude && maximum_altitude - minimum_altitude > 12600.0) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kBounds, "synthetic altitude exceeds FIT range");
  }
  const double altitude_datum = found_altitude ? minimum_altitude : 0.0;
  auto fit_altitude = [&](const TelemetrySample& sample) -> Value {
    if ((sample.validity & kSampleAltitudeValid) == 0U) return std::nullopt;
    const double value = (static_cast<double>(sample.relative_altitude_cm) / 100.0 - altitude_datum + 500.0) * 5.0;
    if (!std::isfinite(value) || value < 0.0 || value >= static_cast<double>(kInvalidU16)) return std::nullopt;
    return static_cast<std::int64_t>(std::llround(value));
  };
  auto sample_timestamp = [&](const TelemetrySample& sample) -> Value {
    const auto timestamp = fit_timestamp(input.start_utc_ms + static_cast<std::int64_t>(sample.wall_ms));
    return timestamp ? Value{static_cast<std::int64_t>(*timestamp)} : std::nullopt;
  };

  const std::vector<Field> file_id = {{0U, 1U, kEnum}, {1U, 2U, kUint16}, {2U, 2U, kUint16},
                                      {3U, 4U, kUint32z}, {4U, 4U, kUint32}};
  const std::vector<Field> creator = {{0U, 2U, kUint16}, {1U, 1U, kUint8}};
  // These field numbers and wire types mirror the public FIT profile. Keep
  // the definitions explicit: a syntactically valid FIT file with shifted
  // field numbers is still semantically corrupt to downstream consumers.
  const std::vector<Field> device_info = {
      {253U, 4U, kUint32}, {0U, 1U, kUint8}, {1U, 1U, kUint8}, {2U, 2U, kUint16},
      {3U, 4U, kUint32z},  {4U, 2U, kUint16}, {5U, 2U, kUint16}, {6U, 1U, kUint8}};
  const std::vector<Field> event = {{253U, 4U, kUint32}, {0U, 1U, kEnum}, {1U, 1U, kEnum}, {3U, 4U, kUint32}};
  const std::vector<Field> record = {{253U, 4U, kUint32}, {7U, 2U, kUint16}, {4U, 1U, kUint8},
                                     {3U, 1U, kUint8}, {6U, 2U, kUint16}, {5U, 4U, kUint32},
                                     {2U, 2U, kUint16}};
  const std::vector<Field> lap = {
      {254U, 2U, kUint16}, {253U, 4U, kUint32}, {0U, 1U, kEnum}, {1U, 1U, kEnum},
      {2U, 4U, kUint32},   {7U, 4U, kUint32},   {8U, 4U, kUint32}, {9U, 4U, kUint32},
      {15U, 1U, kUint8},   {16U, 1U, kUint8},   {17U, 1U, kUint8}, {18U, 1U, kUint8},
      {19U, 2U, kUint16},  {20U, 2U, kUint16},  {25U, 1U, kEnum},  {39U, 1U, kEnum}};
  const std::vector<Field> session = {
      {254U, 2U, kUint16}, {253U, 4U, kUint32}, {0U, 1U, kEnum}, {1U, 1U, kEnum},
      {2U, 4U, kUint32},   {7U, 4U, kUint32},   {8U, 4U, kUint32}, {9U, 4U, kUint32},
      {5U, 1U, kEnum},     {6U, 1U, kEnum},     {16U, 1U, kUint8},  {17U, 1U, kUint8},
      {18U, 1U, kUint8},   {19U, 1U, kUint8},   {20U, 2U, kUint16}, {21U, 2U, kUint16},
      {25U, 2U, kUint16},  {26U, 2U, kUint16}};
  const std::vector<Field> activity = {{253U, 4U, kUint32}, {0U, 4U, kUint32},
                                       {1U, 2U, kUint16}, {2U, 1U, kEnum}, {3U, 1U, kEnum},
                                       {4U, 1U, kEnum}};

  std::vector<std::uint8_t> data_bytes;
  definition(&data_bytes, 0U, 0U, file_id);
  data(&data_bytes, 0U, file_id, {Value{4}, Value{input.identity.manufacturer_id}, Value{input.identity.product_id},
                                  Value{input.identity.serial_number}, Value{static_cast<std::int64_t>(*start_timestamp)}});
  definition(&data_bytes, 1U, 49U, creator);
  data(&data_bytes, 1U, creator, {Value{input.identity.software_version}, Value{input.identity.hardware_version}});
  definition(&data_bytes, 2U, 23U, device_info);
  data(&data_bytes, 2U, device_info,
       {Value{static_cast<std::int64_t>(*start_timestamp)}, Value{0}, Value{5},
        Value{input.identity.manufacturer_id}, Value{input.identity.serial_number},
        Value{input.identity.product_id}, Value{input.identity.software_version},
        Value{input.identity.hardware_version}});
  definition(&data_bytes, 3U, 21U, event);
  for (const auto& item : normalized_events(input, wall_ms)) {
    if (item.wall_ms > wall_ms || item.active_ms > active_ms) {
      return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kInvalidArgument, "FIT event is outside session duration");
    }
    const auto timestamp = fit_timestamp(input.start_utc_ms + static_cast<std::int64_t>(item.wall_ms));
    if (!timestamp) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kTimeUntrusted, "event timestamp is invalid");
    std::uint8_t event_type = 0U;
    // FIT timer events use START=0, STOP=1 and STOP_ALL=4. A pause/resume
    // pair is represented by STOP/START; the final stop uses STOP_ALL, as in
    // Garmin's activity-file example. Do not invent pause/resume enum values.
    if (item.type == SessionEvent::Type::kStop) event_type = 4U;
    else if (item.type == SessionEvent::Type::kPause) event_type = 1U;
    else if (item.type == SessionEvent::Type::kResume) event_type = 0U;
    data(&data_bytes, 3U, event, {Value{static_cast<std::int64_t>(*timestamp)}, Value{0}, Value{event_type}, Value{0}});
  }
  definition(&data_bytes, 4U, 20U, record);
  for (const auto& sample : samples) {
    Value power = (sample.validity & kSamplePowerValid) != 0U && sample.power_w >= 0
                      ? Value{sample.power_w}
                      : std::nullopt;
    const std::int64_t cadence_raw = static_cast<std::int64_t>(std::lround(static_cast<double>(sample.cadence_half_rpm) / 2.0));
    Value cadence = (sample.validity & kSampleCadenceValid) != 0U && cadence_raw < 255
                        ? Value{cadence_raw}
                        : std::nullopt;
    Value heart_rate = (sample.validity & kSampleHeartRateValid) != 0U && sample.heart_rate_bpm < 255U
                           ? Value{sample.heart_rate_bpm}
                           : std::nullopt;
    Value speed = (sample.validity & kSampleSpeedValid) != 0U && sample.speed_mm_s < kInvalidU16
                      ? Value{static_cast<std::int64_t>(sample.speed_mm_s)}
                      : std::nullopt;
    Value distance = (sample.validity & kSampleDistanceValid) != 0U ? Value{sample.distance_cm} : std::nullopt;
    data(&data_bytes, 4U, record, {sample_timestamp(sample), power, cadence, heart_rate, speed, distance, fit_altitude(sample)});
  }
  const Value avg_power = average(samples, kSamplePowerValid, [](const TelemetrySample& sample) { return static_cast<std::int64_t>(sample.power_w); });
  const Value max_power = maximum(samples, kSamplePowerValid, [](const TelemetrySample& sample) { return static_cast<std::int64_t>(sample.power_w); });
  const Value avg_cadence = average(samples, kSampleCadenceValid, [](const TelemetrySample& sample) { return static_cast<std::int64_t>(std::lround(static_cast<double>(sample.cadence_half_rpm) / 2.0)); });
  const Value max_cadence = maximum(samples, kSampleCadenceValid, [](const TelemetrySample& sample) { return static_cast<std::int64_t>(std::lround(static_cast<double>(sample.cadence_half_rpm) / 2.0)); });
  const Value avg_hr = average(samples, kSampleHeartRateValid, [](const TelemetrySample& sample) { return static_cast<std::int64_t>(sample.heart_rate_bpm); });
  const Value max_hr = maximum(samples, kSampleHeartRateValid, [](const TelemetrySample& sample) { return static_cast<std::int64_t>(sample.heart_rate_bpm); });
  std::uint32_t total_distance = 0U;
  for (const auto& sample : samples) if ((sample.validity & kSampleDistanceValid) != 0U) total_distance = std::max(total_distance, sample.distance_cm);
  definition(&data_bytes, 5U, 19U, lap);
  data(&data_bytes, 5U, lap,
       {Value{0}, Value{static_cast<std::int64_t>(*end_timestamp)}, Value{0}, Value{1},
        Value{static_cast<std::int64_t>(*start_timestamp)}, Value{wall_ms}, Value{active_ms},
        Value{total_distance}, Value{avg_hr}, Value{max_hr}, Value{avg_cadence},
        Value{max_cadence}, Value{avg_power}, Value{max_power}, Value{2}, Value{6}});
  definition(&data_bytes, 6U, 18U, session);
  data(&data_bytes, 6U, session,
       {Value{0}, Value{static_cast<std::int64_t>(*end_timestamp)}, Value{0}, Value{1},
        Value{static_cast<std::int64_t>(*start_timestamp)}, Value{wall_ms}, Value{active_ms},
        Value{total_distance}, Value{2}, Value{6}, Value{avg_hr}, Value{max_hr},
        Value{avg_cadence}, Value{max_cadence}, Value{avg_power}, Value{max_power}, Value{0}, Value{1}});
  definition(&data_bytes, 7U, 34U, activity);
  data(&data_bytes, 7U, activity, {Value{static_cast<std::int64_t>(*end_timestamp)}, Value{active_ms}, Value{1}, Value{0}, Value{0}, Value{1}});

  std::vector<std::uint8_t> output;
  output.reserve(kFitHeaderSize + data_bytes.size() + 2U);
  output.push_back(kFitHeaderSize);
  output.push_back(kProtocolVersion);
  put_u16(&output, kProfileVersion);
  put_u32(&output, static_cast<std::uint32_t>(data_bytes.size()));
  output.insert(output.end(), {'.', 'F', 'I', 'T'});
  put_u16(&output, fit_crc(output.data(), output.size()));
  output.insert(output.end(), data_bytes.begin(), data_bytes.end());
  put_u16(&output, fit_crc(output.data(), output.size()));
  return Result<std::vector<std::uint8_t>>::success(std::move(output));
}

bool validate_fit_bytes(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 16U || bytes[0] != kFitHeaderSize || bytes[1] != kProtocolVersion ||
      bytes[8] != '.' || bytes[9] != 'F' || bytes[10] != 'I' || bytes[11] != 'T') return false;
  const std::uint32_t data_size = get_u32(bytes, 4U);
  if (static_cast<std::uint64_t>(kFitHeaderSize) + data_size + 2U != bytes.size()) return false;
  if (get_u16(bytes, 12U) != fit_crc(bytes.data(), 12U)) return false;
  return get_u16(bytes, bytes.size() - 2U) == fit_crc(bytes.data(), bytes.size() - 2U);
}

}  // namespace trainer_loop
