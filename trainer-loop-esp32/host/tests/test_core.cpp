#include "trainer_loop/ble_protocol.h"
#include "trainer_loop/ble_manager.h"
#include "trainer_loop/clock.h"
#include "trainer_loop/erg_policy.h"
#include "trainer_loop/fit_encoder.h"
#include "trainer_loop/fit_store.h"
#include "trainer_loop/hash.h"
#include "trainer_loop/history.h"
#include "trainer_loop/intervals.h"
#include "trainer_loop/journal.h"
#include "trainer_loop/physics.h"
#include "trainer_loop/recorder.h"
#include "trainer_loop/retry_policy.h"
#include "trainer_loop/settings.h"
#include "trainer_loop/setup.h"
#include "trainer_loop/sync.h"
#include "trainer_loop/time_service.h"
#include "trainer_loop/upload_queue.h"
#include "trainer_loop/ui.h"
#include "trainer_loop/workout.h"
#include "trainer_loop/workout_cache.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct FakeTrainer final : trainer_loop::ITrainerController {
  bool fail_commands = false;
  int last_target = -1;
  unsigned acquire_count = 0U;
  unsigned pause_count = 0U;
  unsigned stop_count = 0U;

  trainer_loop::Status acquire_control() override {
    ++acquire_count;
    return fail_commands ? trainer_loop::Status::failure(trainer_loop::ErrorCode::kTransport, "fake acquire failure")
                         : trainer_loop::Status::ok();
  }
  trainer_loop::Status set_target_power(int watts) override {
    last_target = watts;
    return fail_commands ? trainer_loop::Status::failure(trainer_loop::ErrorCode::kTransport, "fake target failure")
                         : trainer_loop::Status::ok();
  }
  trainer_loop::Status start_or_resume() override {
    return fail_commands ? trainer_loop::Status::failure(trainer_loop::ErrorCode::kTransport, "fake start failure")
                         : trainer_loop::Status::ok();
  }
  trainer_loop::Status pause() override {
    ++pause_count;
    return fail_commands ? trainer_loop::Status::failure(trainer_loop::ErrorCode::kTransport, "fake pause failure")
                         : trainer_loop::Status::ok();
  }
  trainer_loop::Status stop() override {
    ++stop_count;
    return trainer_loop::Status::ok();
  }
};

trainer_loop::Workout make_workout() {
  trainer_loop::Workout workout;
  workout.id = "fixture-workout";
  workout.name = "Fixture workout";
  workout.source_hash = "fixture";
  trainer_loop::WorkoutSegment segment;
  segment.duration_ms = 10000U;
  segment.id = "one";
  segment.name = "Steady";
  segment.phase = "Workout";
  segment.start_low = segment.start_high = segment.end_low = segment.end_high = 0.8;
  workout.segments.push_back(segment);
  return workout;
}

trainer_loop::SensorSnapshot fresh_sensors(std::uint64_t now_ms) {
  trainer_loop::SensorSnapshot sensors;
  sensors.trainer_link = trainer_loop::LinkState::kConnected;
  sensors.heart_rate_link = trainer_loop::LinkState::kConnected;
  sensors.telemetry_subscription = true;
  sensors.control_granted = true;
  sensors.power_w = {200, now_ms, true};
  sensors.cadence_rpm = {90.0, now_ms, true};
  sensors.heart_rate_bpm = {145, now_ms, true};
  return sensors;
}

struct FitFieldDefinition {
  std::uint8_t number = 0U;
  std::uint8_t size = 0U;
  std::uint8_t base_type = 0U;
};

struct FitDefinition {
  std::uint16_t global_number = 0U;
  std::vector<FitFieldDefinition> fields;
};

std::uint16_t fit_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t fit_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0U;
  for (unsigned i = 0U; i < 4U; ++i) value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8U * i);
  return value;
}

bool fit_definition_matches(const std::array<std::optional<FitDefinition>, 16U>& definitions,
                            std::uint16_t global_number,
                            std::initializer_list<FitFieldDefinition> expected) {
  for (const auto& definition : definitions) {
    if (!definition.has_value() || definition->global_number != global_number) continue;
    if (definition->fields.size() != expected.size()) return false;
    std::size_t index = 0U;
    for (const FitFieldDefinition& field : expected) {
      const FitFieldDefinition& actual = definition->fields[index++];
      if (actual.number != field.number || actual.size != field.size || actual.base_type != field.base_type) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool fit_profile_definitions_are_valid(const std::vector<std::uint8_t>& bytes) {
  if (!trainer_loop::validate_fit_bytes(bytes) || bytes.size() < 16U) return false;
  const std::size_t data_end = 14U + static_cast<std::size_t>(fit_u32(bytes, 4U));
  std::array<std::optional<FitDefinition>, 16U> definitions;
  std::size_t offset = 14U;
  while (offset < data_end) {
    const std::uint8_t header = bytes[offset++];
    if ((header & 0x40U) != 0U) {
      if ((header & 0x80U) != 0U || offset + 5U > data_end) return false;
      offset += 1U;  // reserved
      if (bytes[offset++] != 0U) return false;  // little-endian architecture
      const std::uint16_t global_number = fit_u16(bytes, offset);
      offset += 2U;
      const std::uint8_t field_count = bytes[offset++];
      if (offset + static_cast<std::size_t>(field_count) * 3U > data_end) return false;
      FitDefinition definition;
      definition.global_number = global_number;
      definition.fields.reserve(field_count);
      for (std::uint8_t index = 0U; index < field_count; ++index) {
        definition.fields.push_back({bytes[offset], bytes[offset + 1U], bytes[offset + 2U]});
        offset += 3U;
      }
      definitions[header & 0x0fU] = std::move(definition);
      continue;
    }
    const auto& definition = definitions[header & 0x0fU];
    if (!definition.has_value()) return false;
    std::size_t record_bytes = 0U;
    for (const FitFieldDefinition& field : definition->fields) record_bytes += field.size;
    if (offset + record_bytes > data_end) return false;
    offset += record_bytes;
  }
  if (offset != data_end) return false;

  return fit_definition_matches(definitions, 0U,
                                {{0U, 1U, 0x00U}, {1U, 2U, 0x84U}, {2U, 2U, 0x84U},
                                 {3U, 4U, 0x8cU}, {4U, 4U, 0x86U}}) &&
         fit_definition_matches(definitions, 23U,
                                {{253U, 4U, 0x86U}, {0U, 1U, 0x02U}, {1U, 1U, 0x02U},
                                 {2U, 2U, 0x84U}, {3U, 4U, 0x8cU}, {4U, 2U, 0x84U},
                                 {5U, 2U, 0x84U}, {6U, 1U, 0x02U}}) &&
         fit_definition_matches(definitions, 19U,
                                {{254U, 2U, 0x84U}, {253U, 4U, 0x86U}, {0U, 1U, 0x00U},
                                 {1U, 1U, 0x00U}, {2U, 4U, 0x86U}, {7U, 4U, 0x86U},
                                 {8U, 4U, 0x86U}, {9U, 4U, 0x86U}, {15U, 1U, 0x02U},
                                 {16U, 1U, 0x02U}, {17U, 1U, 0x02U}, {18U, 1U, 0x02U},
                                 {19U, 2U, 0x84U}, {20U, 2U, 0x84U}, {25U, 1U, 0x00U},
                                 {39U, 1U, 0x00U}}) &&
         fit_definition_matches(definitions, 18U,
                                {{254U, 2U, 0x84U}, {253U, 4U, 0x86U}, {0U, 1U, 0x00U},
                                 {1U, 1U, 0x00U}, {2U, 4U, 0x86U}, {7U, 4U, 0x86U},
                                 {8U, 4U, 0x86U}, {9U, 4U, 0x86U}, {5U, 1U, 0x00U},
                                 {6U, 1U, 0x00U}, {16U, 1U, 0x02U}, {17U, 1U, 0x02U},
                                 {18U, 1U, 0x02U}, {19U, 1U, 0x02U}, {20U, 2U, 0x84U},
                                 {21U, 2U, 0x84U}, {25U, 2U, 0x84U}, {26U, 2U, 0x84U}});
}

std::string temporary_directory() {
  char pattern[] = "/tmp/trainer-loop-test-XXXXXX";
  const int descriptor = ::mkstemp(pattern);
  assert(descriptor >= 0);
  ::close(descriptor);
  assert(std::remove(pattern) == 0);
  assert(::mkdir(pattern, 0700) == 0);
  return pattern;
}

}  // namespace

#include "migration_contracts.h"
#include "ui_settings_contracts.h"
#include "replay_contracts.h"
#include "storage_contracts.h"

int main() {
  migration_contracts();
  ui_settings_contracts();
  replay_contracts();
  storage_contracts();
  assert(trainer_loop::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::uint8_t ftms[] = {0x44, 0x00, 0x2c, 0x01, 0xb6, 0x00, 0xc8, 0x00};
  const auto parsed = trainer_loop::parse_indoor_bike_data(ftms, sizeof(ftms));
  assert(parsed);
  assert(parsed.value->instantaneous_cadence_rpm.has_value());
  assert(parsed.value->instantaneous_power_w.has_value());

  const std::uint8_t more_data[] = {0x45, 0x00, 0xb6, 0x00, 0xc8, 0x00};
  const auto omitted_speed = trainer_loop::parse_indoor_bike_data(more_data, sizeof(more_data));
  assert(omitted_speed && omitted_speed.value->more_data);
  assert(!omitted_speed.value->instantaneous_speed_kph.has_value());

  const std::uint8_t hr_wide[] = {0x03, 0x50, 0x01};
  const auto heart_rate = trainer_loop::parse_heart_rate_measurement(hr_wide, sizeof(hr_wide));
  assert(heart_rate && heart_rate.value->bpm == 336U);
  assert(heart_rate.value->sensor_contact_supported && !heart_rate.value->sensor_contact_detected);

  const auto target = trainer_loop::encode_target_power(200, {true, true, true, true, true, 50, 500, 5});
  assert(target && target.value->at(0) == 0x05U && target.value->at(1) == 0xc8U && target.value->at(2) == 0x00U);
  assert(!trainer_loop::encode_target_power(-1, {true, true, true, true, true, 0, 500, 1}));
  assert(!trainer_loop::encode_target_power(2001, {true, true, true, true, true, 0, 2000, 1}));
  trainer_loop::ControlTransactionGate gate;
  assert(gate.begin(trainer_loop::FtmsOpcode::kRequestControl, 7U, 100U));
  const std::uint8_t ack[] = {0x80, 0x00, 0x01};
  assert(gate.acknowledge(ack, sizeof(ack), 7U, 101U));
  assert(gate.control_valid());
  assert(gate.begin(trainer_loop::FtmsOpcode::kSetTargetPower, 7U, 200U));
  assert(!gate.acknowledge(ack, sizeof(ack), 6U, 201U));
  assert(!gate.poll_timeout(3301U));
  assert(!gate.control_valid());
  assert(gate.begin(trainer_loop::FtmsOpcode::kSetTargetPower, 7U, 4000U));
  const std::uint8_t late_target_ack[] = {0x80, 0x05, 0x01};
  assert(!gate.acknowledge(late_target_ack, sizeof(late_target_ack), 7U, 8001U));
  assert(!gate.pending() && !gate.control_valid());

  const std::string zwo =
      "<?xml version=\"1.0\"?><workout_file><name>Test</name><workout>"
      "<Warmup Duration=\"10\" PowerLow=\"0.5\" PowerHigh=\"0.7\"/>"
      "<Ramp Duration=\"20\" PowerLow=\"0.7\" PowerHigh=\"1.0\"/>"
      "<SteadyState Duration=\"30\" Power=\"0.8\"/>"
      "</workout></workout_file>";
  const auto workout = trainer_loop::parse_zwo(zwo);
  assert(workout);
  assert(workout.value->segments.size() == 3U);
  assert(workout.value->segments.front().kind == trainer_loop::SegmentKind::kRamp);
  const double warmup_target = trainer_loop::resolved_target_range_at(*workout.value, 5000U, 250).low;
  assert(warmup_target > 149.9 && warmup_target < 150.1);
  assert(trainer_loop::resolved_target_range_at(*workout.value, 15000U, 250).low > 150.0);
  const auto cooldown = trainer_loop::parse_zwo(
      "<workout_file><workout><Cooldown Duration=\"10\" PowerLow=\"0.8\" PowerHigh=\"0.4\"/></workout></workout_file>");
  assert(cooldown && cooldown.value->segments.front().kind == trainer_loop::SegmentKind::kRamp);
  assert(trainer_loop::resolved_target_range_at(*cooldown.value, 5000U, 250).low > 149.9 &&
         trainer_loop::resolved_target_range_at(*cooldown.value, 5000U, 250).low < 150.1);
  const auto steady_range = trainer_loop::parse_zwo(
      "<workout_file><workout><SteadyState Duration=\"480\" PowerLow=\"0.88\" PowerHigh=\"0.92\"><textevent message=\"1/3\" duration=\"10\"/></SteadyState></workout></workout_file>");
  assert(steady_range && steady_range.value->segments.size() == 1U &&
         trainer_loop::resolved_target_range_at(*steady_range.value, 0U, 250).low == 220.0 &&
         trainer_loop::resolved_target_range_at(*steady_range.value, 0U, 250).high == 230.0);
  assert(!trainer_loop::parse_zwo("<workout_file><workout><FreeRide Duration=\"1\"/></workout></workout_file>"));
  assert(!trainer_loop::parse_zwo("<workout_file><workout><SteadyState Duration=\"1\" Power=\"nan\"/></workout></workout_file>"));
  assert(!trainer_loop::parse_zwo("<workout_file><workout><SteadyState Duration=\"0.0001\" Power=\"0.5\"/></workout></workout_file>"));
  assert(!trainer_loop::parse_zwo("<other><workout><SteadyState Duration=\"1\" Power=\"0.5\"/></workout></other>"));
  assert(!trainer_loop::parse_zwo("<workout_file><workout><SteadyState Duration=\"1\" Power=\"0.5\" Power=\"0.6\"/></workout></workout_file>"));
  assert(!trainer_loop::parse_zwo(
      "<workout_file><workout><IntervalsT Repeat=\"1\" OnDuration=\"1e308\" OffDuration=\"1\" OnPower=\"0.5\" OffPower=\"0.5\"/></workout></workout_file>"));

  trainer_loop::ProfileSettings profile;
  assert(trainer_loop::validate_profile(profile));
  const auto route = trainer_loop::generate_route(*workout.value, profile.ftp_watts,
                                                   {profile.rider_kg, profile.bike_kg, profile.crr, profile.cda});
  assert(route);
  assert(route.value->grade_percent.size() == 60U);
  assert(trainer_loop::virtual_speed_mps(250, 0.0, {75.0, 8.0, 0.005, 0.32}) > 9.0);
  assert(trainer_loop::virtual_speed_mps(0, -20.0, {75.0, 8.0, 0.005, 0.32}) > 0.0);

  trainer_loop::SessionClocks clocks;
  assert(clocks.start(1000U, {1700000000000LL, true, 1U}));
  assert(clocks.pause(5000U));
  assert(clocks.resume(9000U));
  const auto snapshot = clocks.snapshot(12000U);
  assert(snapshot.wall_ms == 11000U);
  assert(snapshot.active_ms == 7000U);

  assert(snapshot.plan_ms == snapshot.active_ms);
  assert(clocks.snapshot(13000U).wall_ms == 12000U);
  // A regressing monotonic observation must not rewind any clock.
  assert(clocks.snapshot(11000U).wall_ms == 12000U);
  assert(clocks.finish(14000U));
  const auto finished_snapshot = clocks.snapshot(20000U);
  assert(finished_snapshot.wall_ms == 13000U);
  assert(finished_snapshot.active_ms == 9000U);
  assert(!finished_snapshot.running);

  const std::string root = temporary_directory();
  trainer_loop::SessionJournal journal(root + "/journal");
  trainer_loop::JournalManifest journal_manifest{"session-one", "fixture-workout", "Fixture", 0U, 0U, 0U,
                                                0U, 0U, 0U, false, false, 1700000000000LL, true, 3U, ""};
  assert(journal.start(journal_manifest));
  assert(journal.has_current_session());
  trainer_loop::TelemetrySample sample;
  sample.sequence = 0U;
  sample.wall_ms = 1000U;
  sample.active_ms = 1000U;
  sample.plan_ms = 1000U;
  sample.power_w = 0;
  sample.validity = trainer_loop::kSamplePowerValid;
  assert(journal.append_samples({sample}));
  assert(journal.append_event({trainer_loop::SessionEvent::Type::kPause, 1000U, 1000U, 1000U, "user"}));
  const auto recovered = journal.recover_current();
  assert(recovered && recovered.value->samples.size() == 1U && recovered.value->events.size() == 1U);
  trainer_loop::JournalManifest finalized_manifest = journal_manifest;
  finalized_manifest.wall_ms = 1000U;
  finalized_manifest.active_ms = 1000U;
  finalized_manifest.plan_ms = 1000U;
  finalized_manifest.interrupted = true;
  assert(journal.finalize(finalized_manifest));
  assert(!journal.has_current_session());
  const auto published = journal.read_session("session-one");
  assert(published && published.value->manifest.interrupted && published.value->manifest.start_utc_ms == 1700000000000LL &&
         published.value->manifest.time_trusted && published.value->manifest.time_quality == 3U &&
         published.value->samples.size() == 1U);

  trainer_loop::SettingsStore settings;
  trainer_loop::Secrets secrets{"home-wifi", "password", "athlete", "api-key", "admin-secret"};
  const auto prepared_settings = settings.prepare(profile, secrets, settings.current().generation);
  assert(prepared_settings);
  assert(settings.commit(*prepared_settings.value, true, true, true));
  trainer_loop::DeviceIdentity saved_trainer{{1, 2, 3, 4, 5, 6}, 1U, "Zwift Hub", -42,
                                              trainer_loop::PeripheralRole::kTrainer};
  assert(settings.update_device(trainer_loop::PeripheralRole::kTrainer, saved_trainer,
                                settings.current().generation));
  assert(settings.current().trainer_device.has_value());
  assert(trainer_loop::same_identity(*settings.current().trainer_device, saved_trainer));
  trainer_loop::DeviceIdentity wrong_role = saved_trainer;
  wrong_role.role = trainer_loop::PeripheralRole::kHeartRate;
  assert(!settings.update_device(trainer_loop::PeripheralRole::kTrainer, wrong_role,
                                 settings.current().generation));
  const trainer_loop::SettingsGeneration persisted_settings = settings.current();
  trainer_loop::SettingsStore restored_settings;
  assert(restored_settings.restore(persisted_settings));
  assert(restored_settings.current().trainer_device.has_value());
  assert(trainer_loop::same_identity(*restored_settings.current().trainer_device, saved_trainer));
  trainer_loop::SettingsGeneration corrupted_settings = persisted_settings;
  corrupted_settings.trainer_device->name = "different";
  assert(!restored_settings.restore(corrupted_settings));

  trainer_loop::CachedWorkout cached;
  cached.athlete_id = "athlete";
  cached.event_id = "event-1";
  cached.local_date = "2026-09-16";
  cached.timezone = "Europe/Berlin";
  cached.name = workout.value->name;
  cached.source_zwo = zwo;
  cached.workout = *workout.value;
  cached.ftp_used_for_preview = 250;
  cached.downloaded_utc_ms = 1700000000000LL;
  cached.validated_utc_ms = 1700000001000LL;
  trainer_loop::WorkoutCache cache(root + "/cache");
  assert(cache.save(cached));
  const auto cached_back = cache.load("event-1");
  assert(cached_back && cached_back.value->workout.segments.size() == 3U);

  trainer_loop::HistoryStore history(root + "/history.db");
  trainer_loop::SessionSummary summary;
  summary.uuid = "session-one";
  summary.workout_id = "fixture-workout";
  summary.workout_name = "Fixture";
  summary.complete = false;
  summary.interrupted = true;
  summary.sample_count = 1U;
  summary.fit_sha256 = "hash";
  assert(history.save(summary));
  trainer_loop::HistoryStore history_loaded(root + "/history.db");
  assert(history_loaded.load());
  assert(history_loaded.recent(4U).size() == 1U);

  trainer_loop::FitInput fit_input;
  fit_input.start_utc_ms = 1700000000000LL;
  fit_input.time_trusted = true;
  fit_input.wall_duration_ms = 10000U;
  fit_input.active_duration_ms = 7000U;
  sample.validity = trainer_loop::kSamplePowerValid | trainer_loop::kSampleCadenceValid |
                    trainer_loop::kSampleHeartRateValid | trainer_loop::kSampleSpeedValid |
                    trainer_loop::kSampleDistanceValid | trainer_loop::kSampleAltitudeValid;
  sample.power_w = 0;
  sample.cadence_half_rpm = 180U;
  sample.heart_rate_bpm = 145U;
  sample.speed_mm_s = 9000U;
  sample.distance_cm = 1000U;
  sample.relative_altitude_cm = 2500;
  fit_input.samples = {sample};
  const auto fit = trainer_loop::encode_fit(fit_input);
  assert(fit && trainer_loop::validate_fit_bytes(*fit.value) && fit_profile_definitions_are_valid(*fit.value));
  fit_input.samples.push_back(sample);
  fit_input.samples.back().wall_ms = 0U;
  assert(!trainer_loop::encode_fit(fit_input));
  fit_input.samples.pop_back();
  trainer_loop::FitArtifactStore fit_store(root + "/fit");
  const auto artifact = fit_store.publish("session-fit", *fit.value);
  assert(artifact && artifact.value->size == fit.value->size());
  const auto fit_back = fit_store.read(*artifact.value);
  assert(fit_back && *fit_back.value == *fit.value);
  fit_input.time_trusted = false;
  assert(!trainer_loop::encode_fit(fit_input));

  trainer_loop::TimeService time;
  assert(time.set_from_rtc(1700000000000LL, false));
  assert(!time.trusted() && time.snapshot().quality == trainer_loop::TimeQuality::kRtc);
  assert(time.set_from_sntp(1700000000000LL));
  const auto calendar_utc = trainer_loop::utc_ms_from_calendar({2023, 11, 14, 22, 13, 20});
  assert(calendar_utc && *calendar_utc.value == 1700000000000LL);
  assert(!trainer_loop::utc_ms_from_calendar({2024, 2, 29, 23, 59, 59}).error.has_value());
  assert(!trainer_loop::utc_ms_from_calendar({2023, 2, 29, 0, 0, 0}));
  assert(trainer_loop::local_date_for_utc(1700000000000LL, "CET-1CEST,M3.5.0/2,M10.5.0/3") == "2023-11-14");

  trainer_loop::ErgTargetScheduler scheduler;
  assert(scheduler.offer(200, 0U));
  trainer_loop::TargetCommand command;
  assert(scheduler.take_due(0U, &command));
  scheduler.mark_sent(command, 0U);
  assert(scheduler.offer(200, 1000U));
  assert(!scheduler.take_due(1000U, &command));
  assert(scheduler.offer(200, 6000U));
  assert(scheduler.take_due(6000U, &command));
  scheduler.preempt_for_safety();
  assert(!scheduler.offer(100, 7000U));
  scheduler.reset();
  assert(scheduler.offer(100, 7000U));

  assert(trainer_loop::basic_auth_header("a") == "Basic QVBJX0tFWTph");
  assert(trainer_loop::basic_auth_header("key") == "Basic QVBJX0tFWTprZXk=");

  const auto filtered_events = trainer_loop::supported_today_events(
      std::vector<trainer_loop::RemoteWorkoutEvent>{{"a", "Ride", "2026-09-16", "cycling", true},
                                                     {"virtual", "Virtual workout", "2026-09-16", "VirtualRide", true},
                                                     {"b", "Run", "2026-09-16", "running", true},
                                                     {"c", "Old", "2026-09-15", "cycling", true}},
      "2026-09-16");
  assert(filtered_events && filtered_events.value->size() == 2U &&
         filtered_events.value->front().event_id == "a" &&
         filtered_events.value->back().event_id == "virtual");
  profile.ftp_local_override = true;
  const auto local_profile = trainer_loop::apply_remote_profile(profile, {{300}, {80.0}});
  assert(local_profile.ftp_watts == 250 && local_profile.rider_kg == 80.0);

  trainer_loop::SessionJournal recorder_journal(root + "/recorder-journal");
  trainer_loop::Recorder recorder(&recorder_journal);
  assert(recorder.start({"recorded", "fixture-workout", "Fixture", 0U, 0U, 0U, 0U, 0U, 0U, false, false, 0, false, 0U, ""}, 0U));
  for (std::uint32_t i = 0U; i < 5U; ++i) {
    sample.sequence = i;
    sample.wall_ms = i * 1000U;
    assert(recorder.record(sample, i * 1000U));
  }
  assert(recorder.transition({trainer_loop::SessionEvent::Type::kPause, 5000U, 5000U, 5000U, "pause"}));
  const auto recorder_recovered = recorder_journal.recover_current();
  assert(recorder_recovered && recorder_recovered.value->samples.size() == 5U);

  trainer_loop::SessionJournal torn_journal(root + "/torn-journal");
  assert(torn_journal.start({"torn", "fixture-workout", "Fixture", 0U, 0U, 0U, 0U, 0U, 0U, false, false, 0, false, 0U, ""}));
  sample.sequence = 0U;
  assert(torn_journal.append_samples({sample}));
  std::ofstream torn_chunk(root + "/torn-journal/current/chunks/chunk-00000000.bin", std::ios::binary | std::ios::app);
  torn_chunk.put('\x7f');
  torn_chunk.close();
  const auto torn_recovered = torn_journal.recover_current();
  assert(torn_recovered && torn_recovered.value->samples.empty() && torn_recovered.value->ignored_torn_tail);

  trainer_loop::UploadQueue queue(root + "/queue.db");
  assert(queue.load());
  trainer_loop::UploadEntry upload_entry;
  upload_entry.session_uuid = "session-one";
  upload_entry.fit_path = "session-one.fit";
  upload_entry.fit_sha256 = std::string(64U, 'a');
  upload_entry.athlete_id = "athlete";
  upload_entry.filename = "session-one.fit";
  assert(queue.enqueue(upload_entry));
  assert(queue.mark_fit_ready("session-one"));
  assert(queue.mark_uploading("session-one", 0U));
  assert(queue.apply_result("session-one", trainer_loop::classify_upload_response(503, "temporary"), 0U));
  assert(queue.entries().front().state == trainer_loop::UploadState::kRetryWait);
  assert(queue.entries().front().next_attempt_ms >= 27000U && queue.entries().front().next_attempt_ms <= 33000U);
  assert(queue.next_due(queue.entries().front().next_attempt_ms, false).has_value());
  trainer_loop::UploadQueue queue_reloaded(root + "/queue.db");
  assert(queue_reloaded.load() && queue_reloaded.entries().front().state == trainer_loop::UploadState::kRetryWait);
  assert(trainer_loop::classify_upload_response(422, "DUPLICATE activity").classification ==
         trainer_loop::UploadHttpClass::kDuplicate);
  const auto accepted_upload = trainer_loop::classify_upload_response(201, "{\"id\":\"activity-42\"}");
  assert(accepted_upload.classification == trainer_loop::UploadHttpClass::kAccepted &&
         accepted_upload.remote_activity_id == "activity-42");

  const auto setup_credentials = trainer_loop::make_setup_credentials(1234U, 1000U);
  trainer_loop::SetupSession setup;
  assert(setup.begin(setup_credentials, "csrf-token-123456"));
  assert(setup.authorize_request("192.168.4.1", "http://192.168.4.1", "csrf-token-123456", 1001U, true));
  assert(!setup.authorize_request("evil", "http://192.168.4.1", "csrf-token-123456", 1001U, true));
  for (unsigned attempt = 0U; attempt < 7U; ++attempt) {
    assert(!setup.authorize_request("evil", "http://192.168.4.1", "csrf-token-123456", 1001U, true));
  }
  assert(!setup.authorize_request("192.168.4.1", "http://192.168.4.1", "csrf-token-123456", 1001U, true));
  assert(setup.authorize_request("192.168.4.1", "http://192.168.4.1", "csrf-token-123456", 61002U, true));

  trainer_loop::BleConnectionManager connections;
  trainer_loop::DeviceIdentity trainer_identity{{1, 2, 3, 4, 5, 6}, 0U, "Hub", -40, trainer_loop::PeripheralRole::kTrainer};
  assert(connections.select(trainer_identity));
  assert(connections.connected(trainer_loop::PeripheralRole::kTrainer, 0U));
  const std::uint32_t connected_generation = connections.state(trainer_loop::PeripheralRole::kTrainer).generation;
  assert(connections.disconnected(trainer_loop::PeripheralRole::kTrainer, 0U));
  assert(connections.state(trainer_loop::PeripheralRole::kTrainer).generation > connected_generation);
  assert(connections.state(trainer_loop::PeripheralRole::kTrainer).next_attempt_ms == 1000U);
  assert(connections.scan_started(100U));
  assert(connections.scanning());
  trainer_loop::DeviceIdentity heart_rate_identity{{7, 8, 9, 10, 11, 12}, 0U, "HRM", -47,
                                                    trainer_loop::PeripheralRole::kHeartRate};
  assert(connections.select(heart_rate_identity));
  assert(!connections.scanning());
  assert(connections.selected(trainer_loop::PeripheralRole::kHeartRate).has_value());
  assert(connections.scan_started(100U));
  assert(connections.poll_scan(15100U));
  assert(!connections.scanning());

  FakeTrainer fake_trainer;
  trainer_loop::SessionJournal coordinator_journal(root + "/coordinator-journal");
  trainer_loop::SessionCoordinator coordinator(&fake_trainer, &coordinator_journal);
  assert(coordinator.enter_ready());
  assert(coordinator.set_trainer_capabilities({true, true, true, true, true, 0, 500, 5}));
  assert(coordinator.select_workout(make_workout(), profile, false));
  auto sensors = fresh_sensors(1000U);
  assert(coordinator.start(1000U, {1700000000000LL, true, 1U}, sensors, true, "first-athlete"));
  assert(coordinator.snapshot().state == trainer_loop::RideState::kRunning);
  assert(coordinator.tick(1000U, sensors));
  sensors.power_w.timestamp_ms = 5000U;
  assert(coordinator.tick(9000U, sensors));
  assert(coordinator.snapshot().state == trainer_loop::RideState::kPaused);
  assert(!coordinator.resume(9000U, sensors));

  const auto ui = trainer_loop::make_ui_snapshot(coordinator.snapshot(), nullptr);
  assert(ui.safety_message.find("Power stale") != std::string::npos);
  const auto first_workout = trainer_loop::next_today_workout({0U, 3U});
  assert(first_workout.index == 1U && first_workout.count == 3U);
  const auto wrapped_workout = trainer_loop::next_today_workout({2U, 3U});
  assert(wrapped_workout.index == 0U && wrapped_workout.count == 3U);
  const auto empty_workout = trainer_loop::next_today_workout({4U, 0U});
  assert(empty_workout.index == 0U && empty_workout.count == 0U);
  auto progress_workout = make_workout();
  trainer_loop::WorkoutSegment second_segment = progress_workout.segments.front();
  second_segment.id = "two";
  second_segment.name = "Hard";
  progress_workout.segments.push_back(second_segment);
  trainer_loop::CoordinatorSnapshot progress_snapshot;
  progress_snapshot.state = trainer_loop::RideState::kRunning;
  progress_snapshot.clocks.plan_ms = 12500U;
  progress_snapshot.clocks.active_ms = 12500U;
  progress_snapshot.clocks.wall_ms = 12500U;
  const auto progress_ui = trainer_loop::make_ui_snapshot(progress_snapshot, &progress_workout);
  assert(progress_ui.interval_count == 2U && progress_ui.interval_index == 1U);
  assert(progress_ui.interval_remaining_ms == 7500U && progress_ui.workout_progress_percent == 62U);
  assert(coordinator.end_and_save(10000U));
  assert(coordinator.snapshot().state == trainer_loop::RideState::kComplete);
  assert(coordinator.snapshot().complete == false);
  const auto first_ride = coordinator_journal.read_session(coordinator.session_uuid());
  assert(first_ride && first_ride.value->manifest.destination_athlete_id == "first-athlete");
  assert(coordinator.enter_ready());
  assert(coordinator.select_workout(make_workout(), profile, false));
  sensors = fresh_sensors(20000U);
  assert(coordinator.start(20000U, {1700000005000LL, true, 1U}, sensors, true, "second-athlete"));
  assert(coordinator.snapshot().state == trainer_loop::RideState::kRunning);
  assert(coordinator.end_and_save(21000U, true));
  assert(coordinator.snapshot().state == trainer_loop::RideState::kComplete);
  assert(!coordinator.snapshot().complete);
  const auto second_ride = coordinator_journal.read_session(coordinator.session_uuid());
  assert(second_ride && second_ride.value->manifest.destination_athlete_id == "second-athlete");

  trainer_loop::RetryPolicy retry_policy;
  retry_policy.request(1000U);
  assert(retry_policy.pending());
  assert(!retry_policy.due(999U, false, false));
  assert(retry_policy.due(1000U, false, false));
  assert(!retry_policy.due(1000U, true, false));
  retry_policy.begin();
  retry_policy.finish(1000U, false);
  assert(retry_policy.pending());
  assert(!retry_policy.due(30999U, false, false));
  assert(retry_policy.due(31000U, false, false));
  retry_policy.begin();
  retry_policy.finish(31000U, false);
  assert(!retry_policy.due(150999U, false, false));
  assert(retry_policy.due(151000U, false, false));
  retry_policy.begin();
  retry_policy.finish(151000U, true);
  assert(!retry_policy.pending());

  std::filesystem::remove_all(root);

  std::cout << "trainer_core_tests: smoke checks passed\n";
  return 0;
}
