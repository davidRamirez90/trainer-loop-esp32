#include "trainer_loop/ble_protocol.h"

#include "trainer_loop/workout.h"

#include <algorithm>
#include <cmath>

namespace trainer_loop {
namespace {

class Reader {
 public:
  Reader(const std::uint8_t* bytes, std::size_t length) : bytes_(bytes), length_(length) {}

  bool read_u8(std::uint8_t* value) {
    if (bytes_ == nullptr || position_ > length_ || 1U > length_ - position_) return false;
    *value = bytes_[position_++];
    return true;
  }

  bool read_u16(std::uint16_t* value) {
    if (bytes_ == nullptr || position_ > length_ || 2U > length_ - position_) return false;
    *value = static_cast<std::uint16_t>(bytes_[position_]) |
             static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes_[position_ + 1U]) << 8U);
    position_ += 2U;
    return true;
  }

  bool read_i16(std::int16_t* value) {
    std::uint16_t raw = 0U;
    if (!read_u16(&raw)) return false;
    *value = static_cast<std::int16_t>(raw);
    return true;
  }

  bool read_u24(std::uint32_t* value) {
    if (bytes_ == nullptr || position_ > length_ || 3U > length_ - position_) return false;
    *value = static_cast<std::uint32_t>(bytes_[position_]) |
             (static_cast<std::uint32_t>(bytes_[position_ + 1U]) << 8U) |
             (static_cast<std::uint32_t>(bytes_[position_ + 2U]) << 16U);
    position_ += 3U;
    return true;
  }

  std::size_t remaining() const { return length_ - position_; }

  bool skip(std::size_t count) {
    if (count > remaining()) return false;
    position_ += count;
    return true;
  }

 private:
  const std::uint8_t* bytes_ = nullptr;
  std::size_t length_ = 0U;
  std::size_t position_ = 0U;
};

constexpr std::uint16_t kMoreData = 1U << 0U;
constexpr std::uint16_t kAverageSpeed = 1U << 1U;
constexpr std::uint16_t kInstantCadence = 1U << 2U;
constexpr std::uint16_t kAverageCadence = 1U << 3U;
constexpr std::uint16_t kTotalDistance = 1U << 4U;
constexpr std::uint16_t kResistance = 1U << 5U;
constexpr std::uint16_t kInstantPower = 1U << 6U;
constexpr std::uint16_t kAveragePower = 1U << 7U;
constexpr std::uint16_t kEnergy = 1U << 8U;
constexpr std::uint16_t kHeartRate = 1U << 9U;
constexpr std::uint16_t kMet = 1U << 10U;
constexpr std::uint16_t kElapsed = 1U << 11U;
constexpr std::uint16_t kRemaining = 1U << 12U;

bool valid_increment(const TrainerCapabilities& capabilities) {
  return capabilities.power_range_present && capabilities.power_min_watts >= 0 &&
         capabilities.power_max_watts >= capabilities.power_min_watts &&
         capabilities.power_max_watts <= static_cast<int>(kPowerMaxWatts) &&
         capabilities.power_increment_watts > 0 &&
         capabilities.power_increment_watts <= static_cast<int>(kPowerMaxWatts);
}

}  // namespace

Result<IndoorBikeData> parse_indoor_bike_data(const std::uint8_t* bytes, std::size_t length) {
  Reader reader(bytes, length);
  std::uint16_t flags = 0U;
  if (!reader.read_u16(&flags)) {
    return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS packet has no flags");
  }
  IndoorBikeData data;
  data.flags = flags;
  data.more_data = (flags & kMoreData) != 0U;
  std::uint16_t raw_u16 = 0U;
  std::int16_t raw_i16 = 0;
  std::uint32_t raw_u24 = 0U;
  std::uint8_t raw_u8 = 0U;
  if (!data.more_data) {
    if (!reader.read_u16(&raw_u16)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS speed is truncated");
    data.instantaneous_speed_kph = static_cast<double>(raw_u16) / 100.0;
  }
  if ((flags & kAverageSpeed) != 0U) {
    if (!reader.read_u16(&raw_u16)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS average speed is truncated");
    data.average_speed_kph = static_cast<double>(raw_u16) / 100.0;
  }
  if ((flags & kInstantCadence) != 0U) {
    if (!reader.read_u16(&raw_u16)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS cadence is truncated");
    data.instantaneous_cadence_rpm = static_cast<double>(raw_u16) / 2.0;
  }
  if ((flags & kAverageCadence) != 0U) {
    if (!reader.read_u16(&raw_u16)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS average cadence is truncated");
    data.average_cadence_rpm = static_cast<double>(raw_u16) / 2.0;
  }
  if ((flags & kTotalDistance) != 0U) {
    if (!reader.read_u24(&raw_u24)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS distance is truncated");
    data.total_distance_m = raw_u24;
  }
  if ((flags & kResistance) != 0U) {
    if (!reader.read_i16(&raw_i16)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS resistance is truncated");
    data.resistance_level = raw_i16;
  }
  if ((flags & kInstantPower) != 0U) {
    if (!reader.read_i16(&raw_i16)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS power is truncated");
    data.instantaneous_power_w = raw_i16;
  }
  if ((flags & kAveragePower) != 0U) {
    if (!reader.read_i16(&raw_i16)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS average power is truncated");
    data.average_power_w = raw_i16;
  }
  if ((flags & kEnergy) != 0U) {
    if (!reader.read_u16(&raw_u16) || !reader.skip(3U)) {
      return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS energy fields are truncated");
    }
    data.total_energy_kj = raw_u16;
  }
  if ((flags & kHeartRate) != 0U) {
    if (!reader.read_u8(&raw_u8)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS heart rate is truncated");
    data.heart_rate_bpm = raw_u8;
  }
  if ((flags & kMet) != 0U) {
    if (!reader.read_u8(&raw_u8)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS MET is truncated");
    data.metabolic_equivalent = static_cast<double>(raw_u8) / 10.0;
  }
  if ((flags & kElapsed) != 0U) {
    if (!reader.read_u16(&raw_u16)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS elapsed time is truncated");
    data.elapsed_time_s = raw_u16;
  }
  if ((flags & kRemaining) != 0U) {
    if (!reader.read_u16(&raw_u16)) return Result<IndoorBikeData>::failure(ErrorCode::kMalformedInput, "FTMS remaining time is truncated");
    data.remaining_time_s = raw_u16;
  }
  if (reader.remaining() != 0U) {
    // Extra bytes are permitted by future revisions, but not needed for the
    // current contract. They are ignored only after all flagged fields have
    // been consumed, so offsets cannot drift.
  }
  return Result<IndoorBikeData>::success(std::move(data));
}

Status apply_indoor_bike_data(const IndoorBikeData& data, std::uint64_t timestamp_ms,
                              SensorSnapshot* snapshot) {
  if (snapshot == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "snapshot is null");
  if (data.instantaneous_speed_kph.has_value()) {
    snapshot->trainer_speed_kph = {*data.instantaneous_speed_kph, timestamp_ms, true};
  }
  if (data.instantaneous_cadence_rpm.has_value()) {
    snapshot->cadence_rpm = {*data.instantaneous_cadence_rpm, timestamp_ms, true};
  }
  if (data.total_distance_m.has_value()) {
    snapshot->distance_m = {*data.total_distance_m, timestamp_ms, true};
  }
  if (data.instantaneous_power_w.has_value()) {
    snapshot->power_w = {*data.instantaneous_power_w, timestamp_ms, true};
  }
  if (data.heart_rate_bpm.has_value()) {
    snapshot->heart_rate_bpm = {static_cast<int>(*data.heart_rate_bpm), timestamp_ms, true};
  }
  snapshot->trainer_link = LinkState::kConnected;
  snapshot->telemetry_subscription = true;
  return Status::ok();
}

Result<HeartRateMeasurement> parse_heart_rate_measurement(const std::uint8_t* bytes,
                                                          std::size_t length) {
  Reader reader(bytes, length);
  std::uint8_t flags = 0U;
  if (!reader.read_u8(&flags)) return Result<HeartRateMeasurement>::failure(ErrorCode::kMalformedInput, "HRS packet has no flags");
  HeartRateMeasurement measurement;
  const bool wide_value = (flags & 0x01U) != 0U;
  if (wide_value) {
    std::uint16_t value = 0U;
    if (!reader.read_u16(&value)) return Result<HeartRateMeasurement>::failure(ErrorCode::kMalformedInput, "HRS wide value is truncated");
    measurement.bpm = value;
  } else {
    std::uint8_t value = 0U;
    if (!reader.read_u8(&value)) return Result<HeartRateMeasurement>::failure(ErrorCode::kMalformedInput, "HRS value is truncated");
    measurement.bpm = value;
  }
  // HRS uses bit 1 to advertise contact support and bit 2 for the current
  // contact state. Bit 2 on its own is reserved, not evidence of support.
  measurement.sensor_contact_supported = (flags & 0x02U) != 0U;
  measurement.sensor_contact_detected = measurement.sensor_contact_supported && (flags & 0x04U) != 0U;
  measurement.energy_expended_present = (flags & 0x08U) != 0U;
  measurement.rr_interval_present = (flags & 0x10U) != 0U;
  if (measurement.energy_expended_present && !reader.skip(2U)) {
    return Result<HeartRateMeasurement>::failure(ErrorCode::kMalformedInput, "HRS energy is truncated");
  }
  if (measurement.rr_interval_present && (reader.remaining() % 2U != 0U)) {
    return Result<HeartRateMeasurement>::failure(ErrorCode::kMalformedInput, "HRS RR data is truncated");
  }
  return Result<HeartRateMeasurement>::success(measurement);
}

Status apply_heart_rate_measurement(const HeartRateMeasurement& measurement,
                                    std::uint64_t timestamp_ms,
                                    SensorSnapshot* snapshot) {
  if (snapshot == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "snapshot is null");
  snapshot->heart_rate_bpm = {static_cast<int>(measurement.bpm), timestamp_ms, true};
  snapshot->heart_rate_link = LinkState::kConnected;
  return Status::ok();
}

std::vector<std::uint8_t> encode_request_control() {
  return {static_cast<std::uint8_t>(FtmsOpcode::kRequestControl)};
}

std::vector<std::uint8_t> encode_start_resume() {
  return {static_cast<std::uint8_t>(FtmsOpcode::kStartResume)};
}

std::vector<std::uint8_t> encode_stop_pause(FtmsStopParameter parameter) {
  return {static_cast<std::uint8_t>(FtmsOpcode::kStopPause), static_cast<std::uint8_t>(parameter)};
}

Result<std::vector<std::uint8_t>> encode_target_power(int watts,
                                                       const TrainerCapabilities& capabilities) {
  if (!valid_increment(capabilities)) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kUnsupported, "trainer power range is unavailable");
  }
  if (watts < static_cast<int>(kPowerMinWatts) || watts > static_cast<int>(kPowerMaxWatts)) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kBounds, "target is outside product power bounds");
  }
  if (watts < capabilities.power_min_watts || watts > capabilities.power_max_watts) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kBounds, "target is outside trainer power range");
  }
  const int delta = watts - capabilities.power_min_watts;
  const int increment = capabilities.power_increment_watts;
  const int quantized = capabilities.power_min_watts + ((delta + increment / 2) / increment) * increment;
  if (quantized < capabilities.power_min_watts || quantized > capabilities.power_max_watts) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kBounds, "quantized target is outside trainer power range");
  }
  return Result<std::vector<std::uint8_t>>::success({
      static_cast<std::uint8_t>(FtmsOpcode::kSetTargetPower),
      static_cast<std::uint8_t>(quantized & 0xff),
      static_cast<std::uint8_t>((quantized >> 8) & 0xff)});
}

Result<TrainerCapabilities> parse_supported_power_range(const std::uint8_t* bytes, std::size_t length) {
  Reader reader(bytes, length);
  std::int16_t minimum = 0;
  std::int16_t maximum = 0;
  std::int16_t increment = 0;
  if (!reader.read_i16(&minimum) || !reader.read_i16(&maximum) || !reader.read_i16(&increment)) {
    return Result<TrainerCapabilities>::failure(ErrorCode::kMalformedInput, "FTMS power range is truncated");
  }
  if (minimum < 0 || maximum < minimum || maximum > static_cast<std::int16_t>(kPowerMaxWatts) || increment <= 0) {
    return Result<TrainerCapabilities>::failure(ErrorCode::kBounds, "FTMS power range is invalid");
  }
  TrainerCapabilities capabilities;
  capabilities.ftms_present = true;
  capabilities.power_range_present = true;
  capabilities.power_min_watts = minimum;
  capabilities.power_max_watts = maximum;
  capabilities.power_increment_watts = increment;
  return Result<TrainerCapabilities>::success(capabilities);
}

Status validate_workout_power_range(const Workout& workout, int ftp_watts,
                                    const TrainerCapabilities& capabilities) {
  const Status workout_status = validate_workout(workout, ftp_watts);
  if (!workout_status) return workout_status;
  if (!valid_increment(capabilities)) return Status::failure(ErrorCode::kUnsupported, "trainer power range is unavailable");
  for (std::uint32_t at_ms = 0U; at_ms < workout.duration_ms(); at_ms += 1000U) {
    const TargetRange target = resolved_target_range_at(workout, at_ms, ftp_watts);
    const int midpoint_watts = static_cast<int>(std::lround((target.low + target.high) / 2.0));
    const auto encoded = encode_target_power(midpoint_watts, capabilities);
    if (!encoded) return Status::failure(encoded.error->code, encoded.error->message);
    const int quantized = static_cast<int>(encoded.value->at(1)) | (static_cast<int>(encoded.value->at(2)) << 8);
    if (std::abs(quantized - midpoint_watts) > capabilities.power_increment_watts) {
      return Status::failure(ErrorCode::kBounds, "trainer quantization materially changes workout target");
    }
  }
  const TargetRange end_target = resolved_target_range_at(workout, workout.duration_ms(), ftp_watts);
  if (!encode_target_power(static_cast<int>(std::lround((end_target.low + end_target.high) / 2.0)), capabilities)) {
    return Status::failure(ErrorCode::kBounds, "workout end target is outside trainer power range");
  }
  return Status::ok();
}

Result<FtmsResponse> parse_ftms_response(const std::uint8_t* bytes, std::size_t length) {
  if (bytes == nullptr || length < 3U || bytes[0] != 0x80U) {
    return Result<FtmsResponse>::failure(ErrorCode::kMalformedInput, "invalid FTMS response");
  }
  FtmsResponse response;
  const std::uint8_t opcode = bytes[1];
  if (opcode != static_cast<std::uint8_t>(FtmsOpcode::kRequestControl) &&
      opcode != static_cast<std::uint8_t>(FtmsOpcode::kSetTargetPower) &&
      opcode != static_cast<std::uint8_t>(FtmsOpcode::kStartResume) &&
      opcode != static_cast<std::uint8_t>(FtmsOpcode::kStopPause)) {
    return Result<FtmsResponse>::failure(ErrorCode::kUnsupported, "unknown FTMS response opcode");
  }
  response.request_opcode = static_cast<FtmsOpcode>(opcode);
  if (bytes[2] < static_cast<std::uint8_t>(FtmsResultCode::kSuccess) ||
      bytes[2] > static_cast<std::uint8_t>(FtmsResultCode::kControlNotPermitted)) {
    return Result<FtmsResponse>::failure(ErrorCode::kUnsupported, "unknown FTMS result code");
  }
  response.result = static_cast<FtmsResultCode>(bytes[2]);
  return Result<FtmsResponse>::success(response);
}

Status ControlTransactionGate::begin(FtmsOpcode opcode, std::uint32_t generation,
                                     std::uint64_t now_ms, std::uint32_t deadline_ms) {
  if (pending_.has_value()) return Status::failure(ErrorCode::kConflict, "FTMS command already pending");
  if (deadline_ms == 0U) return Status::failure(ErrorCode::kInvalidArgument, "FTMS deadline is zero");
  if (now_ms > UINT64_MAX - deadline_ms) {
    return Status::failure(ErrorCode::kOverflow, "FTMS deadline overflows monotonic clock");
  }
  pending_ = Pending{opcode, generation, now_ms + deadline_ms};
  return Status::ok();
}

Result<FtmsResponse> ControlTransactionGate::acknowledge(const std::uint8_t* bytes,
                                                         std::size_t length,
                                                         std::uint32_t generation,
                                                         std::uint64_t now_ms) {
  const auto response = parse_ftms_response(bytes, length);
  if (!response) return response;
  if (!pending_.has_value()) return Result<FtmsResponse>::failure(ErrorCode::kConflict, "unsolicited FTMS response");
  if (generation != pending_->generation || response.value->request_opcode != pending_->opcode) {
    return Result<FtmsResponse>::failure(ErrorCode::kConflict, "stale or unmatched FTMS response");
  }
  if (now_ms > pending_->deadline_ms) {
    pending_.reset();
    control_valid_ = false;
    return Result<FtmsResponse>::failure(ErrorCode::kTimeout, "late FTMS acknowledgement");
  }
  const FtmsResponse accepted = *response.value;
  pending_.reset();
  control_valid_ = accepted.result == FtmsResultCode::kSuccess;
  if (!control_valid_) {
    return Result<FtmsResponse>::failure(ErrorCode::kTransport, "FTMS command was rejected");
  }
  return Result<FtmsResponse>::success(accepted);
}

Status ControlTransactionGate::poll_timeout(std::uint64_t now_ms) {
  if (!pending_.has_value()) return Status::ok();
  if (now_ms <= pending_->deadline_ms) return Status::ok();
  pending_.reset();
  control_valid_ = false;
  return Status::failure(ErrorCode::kTimeout, "FTMS acknowledgement timed out");
}

void ControlTransactionGate::invalidate() {
  pending_.reset();
  control_valid_ = false;
}

}  // namespace trainer_loop
