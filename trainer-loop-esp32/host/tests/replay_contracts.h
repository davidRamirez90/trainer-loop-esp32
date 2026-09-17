#include "trainer_replay.h"
void replay_contracts() {
  using namespace trainer_loop;
  TrainerReplay replay;
  assert(replay.connect());
  const auto old_generation = replay.generation();
  assert(replay.frame(PeripheralRole::kTrainer, old_generation, {0x40,0,0,0,200,0}, 1000));
  assert(replay.sensors.power_w.value == 200);
  assert(replay.frame(PeripheralRole::kHeartRate, old_generation, {0x06,145}, 1000));
  assert(replay.sensors.heart_rate_bpm.value == 145);
  assert(replay.acquire_control());
  replay.reply = TrainerReplay::Reply::kNack;
  assert(!replay.set_target_power(200));
  replay.reply = TrainerReplay::Reply::kLate;
  assert(!replay.acquire_control());
  replay.reply = TrainerReplay::Reply::kTimeout;
  assert(!replay.acquire_control());
  replay.reply = TrainerReplay::Reply::kAck;
  assert(replay.acquire_control());
  assert(replay.pause()); assert(replay.trace.back() == encode_stop_pause(FtmsStopParameter::kPause));
  assert(replay.stop()); assert(replay.trace.back() == encode_stop_pause(FtmsStopParameter::kStop));
  replay.disconnect(); assert(replay.connect());
  assert(!replay.frame(PeripheralRole::kTrainer, old_generation, {0x40,0,0,0,250,0}, 2000));
  assert(!replay.sensors.power_w.valid);
  assert(replay.frame(PeripheralRole::kTrainer, replay.generation(), {0x40,0,0,0,0,0}, 2000));
  assert(replay.sensors.power_w.valid && replay.sensors.power_w.value == 0);
  assert(!replay.frame(PeripheralRole::kTrainer, replay.generation(), {0x40,0}, 2000));
  assert(!replay.set_target_power(2001));
  const auto root = temporary_directory();
  SessionJournal journal(root + "/replay");
  SessionCoordinator ride(&replay, &journal);
  assert(ride.enter_ready());
  assert(ride.set_trainer_capabilities(replay.capabilities));
  assert(ride.select_workout(make_workout(), ProfileSettings{}, false));
  assert(ride.start(2000, {1700000000000LL,true,3}, replay.sensors, true));
  replay.reply = TrainerReplay::Reply::kNack;
  assert(ride.tick(3000, replay.sensors));
  assert(ride.snapshot().state == RideState::kPaused && ride.snapshot().resistance_unknown);
  replay.reply = TrainerReplay::Reply::kAck;
  assert(replay.frame(PeripheralRole::kTrainer, replay.generation(), {0x40,0,0,0,100,0}, 4000));
  assert(ride.tick(4000, replay.sensors));
  assert(ride.snapshot().state == RideState::kPaused);
  assert(ride.resume(4000, replay.sensors));
  assert(ride.end_and_save(4500));
  assert(replay.trace.back() == encode_stop_pause(FtmsStopParameter::kStop));
  std::filesystem::remove_all(root);
  for (const std::vector<std::uint8_t>& frame : {
       std::vector<std::uint8_t>{0,120}, {6,120}, {7,0x50,1}, {0x18,120,0,0,0,4}}) {
    assert(parse_heart_rate_measurement(frame.data(), frame.size()));
  }
  const std::uint8_t missing_wide[] = {1,120};
  assert(!parse_heart_rate_measurement(missing_wide, sizeof(missing_wide)));
  // Every supported flag combination with a complete payload, and every
  // truncated prefix. No live radio or measured resistance is implied.
  for (unsigned flags = 0; flags < 8192; ++flags) {
    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(flags), static_cast<std::uint8_t>(flags >> 8)};
    const unsigned widths[] = {0,2,2,2,3,2,2,2,5,1,1,2,2};
    if (!(flags & 1)) bytes.insert(bytes.end(), 2, 0);
    for (unsigned bit = 1; bit < 13; ++bit) if (flags & (1U << bit)) bytes.insert(bytes.end(), widths[bit], 0);
    assert(parse_indoor_bike_data(bytes.data(), bytes.size()));
    for (std::size_t length = 0; length < bytes.size(); ++length)
      assert(!parse_indoor_bike_data(bytes.data(), length));
  }
}
