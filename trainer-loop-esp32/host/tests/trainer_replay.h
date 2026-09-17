#pragma once
// Laptop-only transport simulator. It runs production parsers, connection
// generations and transaction gates; it makes no claim about physical ERG.
class TrainerReplay final : public trainer_loop::ITrainerController {
 public:
  enum class Reply { kAck, kNack, kLate, kTimeout };
  Reply reply = Reply::kAck;
  trainer_loop::SensorSnapshot sensors;
  trainer_loop::TrainerCapabilities capabilities{true,true,true,true,true,0,500,5};
  std::vector<std::vector<std::uint8_t>> trace;
  bool connect() {
    using namespace trainer_loop;
    for (auto role : {PeripheralRole::kTrainer, PeripheralRole::kHeartRate}) {
      if (!links.selected(role)) {
        DeviceIdentity peer{{1,2,3,4,5,6},0,"fixture",-40,role};
        if (!links.select(peer)) return false;
      }
      if (!links.connected(role, now) || !links.services_ready(role)) return false;
    }
    sensors.trainer_link = LinkState::kConnected;
    sensors.heart_rate_link = LinkState::kConnected;
    sensors.telemetry_subscription = true;
    return true;
  }
  void disconnect() {
    using namespace trainer_loop;
    (void)links.disconnected(PeripheralRole::kTrainer, now);
    (void)links.disconnected(PeripheralRole::kHeartRate, now);
    gate.invalidate(); sensors = SensorSnapshot{};
  }
  std::uint32_t generation() const { return links.state(trainer_loop::PeripheralRole::kTrainer).generation; }
  bool frame(trainer_loop::PeripheralRole role, std::uint32_t gen,
             const std::vector<std::uint8_t>& bytes, std::uint64_t timestamp) {
    using namespace trainer_loop;
    if (!notification_is_current(links.state(role), gen)) return false;
    if (role == PeripheralRole::kTrainer) {
      const auto parsed = parse_indoor_bike_data(bytes.data(), bytes.size());
      return parsed && static_cast<bool>(apply_indoor_bike_data(*parsed.value, timestamp, &sensors));
    }
    const auto parsed = parse_heart_rate_measurement(bytes.data(), bytes.size());
    return parsed && static_cast<bool>(apply_heart_rate_measurement(*parsed.value, timestamp, &sensors));
  }
  trainer_loop::Status acquire_control() override { return send(trainer_loop::encode_request_control()); }
  trainer_loop::Status set_target_power(int watts) override {
    auto bytes = trainer_loop::encode_target_power(watts, capabilities);
    return bytes ? send(*bytes.value) : trainer_loop::Status::failure(bytes.error->code, bytes.error->message);
  }
  trainer_loop::Status start_or_resume() override { return send(trainer_loop::encode_start_resume()); }
  trainer_loop::Status pause() override { return send(trainer_loop::encode_stop_pause(trainer_loop::FtmsStopParameter::kPause)); }
  trainer_loop::Status stop() override { return send(trainer_loop::encode_stop_pause(trainer_loop::FtmsStopParameter::kStop)); }
 private:
  trainer_loop::BleConnectionManager links;
  trainer_loop::ControlTransactionGate gate;
  std::uint64_t now = 0;
  trainer_loop::Status send(const std::vector<std::uint8_t>& bytes) {
    using namespace trainer_loop;
    trace.push_back(bytes);
    auto status = gate.begin(static_cast<FtmsOpcode>(bytes.front()), generation(), now);
    if (!status) return status;
    if (reply == Reply::kTimeout) { now += 3001; return gate.poll_timeout(now); }
    now += reply == Reply::kLate ? 3001 : 1;
    const std::uint8_t ack[] = {0x80, bytes.front(), static_cast<std::uint8_t>(reply == Reply::kNack ? 4 : 1)};
    const auto result = gate.acknowledge(ack, sizeof(ack), generation(), now);
    return result ? Status::ok() : Status::failure(result.error->code, result.error->message);
  }
};
