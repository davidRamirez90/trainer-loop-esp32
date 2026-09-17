#include "trainer_loop/fit_encoder.h"
#include "trainer_loop/journal.h"
#include <cassert>
#include <filesystem>
#include <fstream>
using namespace trainer_loop;
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::filesystem::path root(argv[1]);
  std::filesystem::create_directories(root);
  for (const std::string name : {"complete", "partial", "paused", "recovered"}) {
    FitInput input;
    input.start_utc_ms = 1700000000000LL; input.time_trusted = true;
    input.wall_duration_ms = name == "complete" ? 10000 : name == "partial" ? 3000 : 8000;
    input.active_duration_ms = name == "paused" || name == "recovered" ? 4000 : input.wall_duration_ms;
    TelemetrySample zero;
    zero.sequence = 0; zero.wall_ms = 1000; zero.active_ms = 1000; zero.plan_ms = 1000;
    zero.validity = kSamplePowerValid | kSampleSpeedValid | kSampleDistanceValid | kSampleAltitudeValid;
    zero.relative_altitude_cm = 1000;
    TelemetrySample missing; missing.sequence = 1; missing.wall_ms = 2000; missing.active_ms = 2000;
    input.samples = {zero, missing};
    if (name == "paused" || name == "recovered") {
      input.events = {{name == "recovered" ? SessionEvent::Type::kTrainerDisconnected : SessionEvent::Type::kPause,
                        2000,2000,2000,"pause"},
                      {SessionEvent::Type::kResume,6000,2000,2000,"resume"}};
      auto last = zero; last.sequence = 2; last.wall_ms = 7000; last.active_ms = 3000;
      last.power_w = 200; last.distance_cm = 1200; last.relative_altitude_cm = -1000;
      input.samples.push_back(last);
    }
    if (name == "recovered") {
      const auto journal_dir = root / "recovery-journal";
      // Test-owned temporary fixture directory only.
      std::filesystem::remove_all(journal_dir);
      SessionJournal journal(journal_dir.string());
      JournalManifest manifest;
      manifest.uuid = "recovered"; manifest.workout_name = "Recovered fixture";
      manifest.start_utc_ms = input.start_utc_ms; manifest.time_trusted = true;
      assert(journal.start(manifest));
      assert(journal.append_samples(input.samples));
      for (const auto& event : input.events) assert(journal.append_event(event));

      const auto recovered = SessionJournal(journal_dir.string()).recover_current();
      assert(recovered);
      input.samples = recovered.value->samples; input.events = recovered.value->events;
      input.wall_duration_ms = recovered.value->manifest.wall_ms;
      input.active_duration_ms = recovered.value->manifest.active_ms;
    }
    const auto bytes = encode_fit(input); assert(bytes);
    assert(encode_fit(input).value == bytes.value);
    std::ofstream output(root / (name + ".fit"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.value->data()), static_cast<std::streamsize>(bytes.value->size()));
    assert(output);
  }
}
