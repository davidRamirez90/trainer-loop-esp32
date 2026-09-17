#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"
#include "trainer_loop/workout.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace trainer_loop {

namespace ble_uuids {
constexpr char kFitnessMachineService[] = "00001826-0000-1000-8000-00805f9b34fb";
constexpr char kHeartRateService[] = "0000180d-0000-1000-8000-00805f9b34fb";
constexpr char kDeviceInformationService[] = "0000180a-0000-1000-8000-00805f9b34fb";
constexpr char kBatteryService[] = "0000180f-0000-1000-8000-00805f9b34fb";
constexpr char kIndoorBikeData[] = "00002ad2-0000-1000-8000-00805f9b34fb";
constexpr char kFitnessMachineFeature[] = "00002acc-0000-1000-8000-00805f9b34fb";
constexpr char kSupportedPowerRange[] = "00002ad8-0000-1000-8000-00805f9b34fb";
constexpr char kFitnessMachineControlPoint[] = "00002ad9-0000-1000-8000-00805f9b34fb";
constexpr char kFitnessMachineStatus[] = "00002ada-0000-1000-8000-00805f9b34fb";
constexpr char kHeartRateMeasurement[] = "00002a37-0000-1000-8000-00805f9b34fb";
}  // namespace ble_uuids

struct IndoorBikeData {
  std::uint16_t flags = 0U;
  bool more_data = false;
  std::optional<double> instantaneous_speed_kph;
  std::optional<double> average_speed_kph;
  std::optional<double> instantaneous_cadence_rpm;
  std::optional<double> average_cadence_rpm;
  std::optional<std::uint32_t> total_distance_m;
  std::optional<std::int16_t> resistance_level;
  std::optional<std::int16_t> instantaneous_power_w;
  std::optional<std::int16_t> average_power_w;
  std::optional<std::uint16_t> total_energy_kj;
  std::optional<std::uint8_t> heart_rate_bpm;
  std::optional<double> metabolic_equivalent;
  std::optional<std::uint16_t> elapsed_time_s;
  std::optional<std::uint16_t> remaining_time_s;
};

Result<IndoorBikeData> parse_indoor_bike_data(const std::uint8_t* bytes, std::size_t length);
Status apply_indoor_bike_data(const IndoorBikeData& data, std::uint64_t timestamp_ms,
                              SensorSnapshot* snapshot);

struct HeartRateMeasurement {
  std::uint16_t bpm = 0U;
  bool sensor_contact_supported = false;
  bool sensor_contact_detected = false;
  bool energy_expended_present = false;
  bool rr_interval_present = false;
};

Result<HeartRateMeasurement> parse_heart_rate_measurement(const std::uint8_t* bytes,
                                                          std::size_t length);
Status apply_heart_rate_measurement(const HeartRateMeasurement& measurement,
                                    std::uint64_t timestamp_ms,
                                    SensorSnapshot* snapshot);

enum class FtmsOpcode : std::uint8_t {
  kRequestControl = 0x00,
  kSetTargetPower = 0x05,
  kStartResume = 0x07,
  kStopPause = 0x08,
};

enum class FtmsStopParameter : std::uint8_t { kStop = 0x01, kPause = 0x02 };

std::vector<std::uint8_t> encode_request_control();
std::vector<std::uint8_t> encode_start_resume();
std::vector<std::uint8_t> encode_stop_pause(FtmsStopParameter parameter);
Result<std::vector<std::uint8_t>> encode_target_power(int watts,
                                                       const TrainerCapabilities& capabilities);
Result<TrainerCapabilities> parse_supported_power_range(const std::uint8_t* bytes, std::size_t length);
Status validate_workout_power_range(const Workout& workout, int ftp_watts,
                                    const TrainerCapabilities& capabilities);

enum class FtmsResultCode : std::uint8_t {
  kSuccess = 0x01,
  kUnsupported = 0x02,
  kInvalidParameter = 0x03,
  kOperationFailed = 0x04,
  kControlNotPermitted = 0x05,
};

struct FtmsResponse {
  FtmsOpcode request_opcode = FtmsOpcode::kRequestControl;
  FtmsResultCode result = FtmsResultCode::kOperationFailed;
};

Result<FtmsResponse> parse_ftms_response(const std::uint8_t* bytes, std::size_t length);

class ControlTransactionGate {
 public:
  struct Pending {
    FtmsOpcode opcode = FtmsOpcode::kRequestControl;
    std::uint32_t generation = 0U;
    std::uint64_t deadline_ms = 0U;
  };

  Status begin(FtmsOpcode opcode, std::uint32_t generation, std::uint64_t now_ms,
               std::uint32_t deadline_ms = 3000U);
  Result<FtmsResponse> acknowledge(const std::uint8_t* bytes, std::size_t length,
                                   std::uint32_t generation, std::uint64_t now_ms);
  Status poll_timeout(std::uint64_t now_ms);
  void invalidate();
  bool pending() const { return pending_.has_value(); }
  bool control_valid() const { return control_valid_; }
  const std::optional<Pending>& pending_command() const { return pending_; }

 private:
  std::optional<Pending> pending_;
  bool control_valid_ = false;
};

}  // namespace trainer_loop
