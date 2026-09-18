#include "trainer_loop/storage_io.h"
#include "../../main/storage_mount.h"
void storage_contracts() {
  using namespace trainer_loop;
  // Existing LittleFS content must never authorize formatting after a mount
  // failure; only a demonstrably erased partition may take the first-boot path.
  assert(!storage_mount_format_allowed(false));
  assert(storage_mount_format_allowed(true));
  const auto root = temporary_directory();
  SessionJournal journal(root + "/journal");
  JournalManifest manifest;
  manifest.uuid = "storage"; manifest.workout_name = "Storage fixture";
  manifest.start_utc_ms = 1700000000000LL; manifest.time_trusted = true;
  manifest.destination_athlete_id = "destination-athlete";
  assert(journal.start(manifest));
  TelemetrySample first; first.sequence = 0; first.wall_ms = 1000; first.active_ms = first.plan_ms = 1000;
  assert(journal.append_samples({first}));
  assert(journal.append_event({SessionEvent::Type::kStart,0,0,0,"start"}));
  assert(journal.recover_current().value->manifest.destination_athlete_id == "destination-athlete");
  storage_test_write_budget = 10;
  assert(!journal.append_event({SessionEvent::Type::kPause,1000,1000,1000,"pause"}));
  storage_test_write_budget.reset();
  assert(journal.append_event({SessionEvent::Type::kPause,1000,1000,1000,"pause"}));
  assert(journal.recover_current().value->events.size() == 2);
  auto second = first; second.sequence = 1; second.wall_ms = second.active_ms = second.plan_ms = 2000;
  for (std::size_t budget : {0U, 10U, 68U, 80U}) {
    storage_test_write_budget = budget; // chunk is 68 bytes, then manifest.
    assert(!journal.append_samples({second}));
    storage_test_write_budget.reset();
    const auto reboot = SessionJournal(root + "/journal").recover_current();
    assert(reboot && reboot.value->samples.size() == 1);
  }
  assert(journal.append_samples({second}));
  assert(!journal.append_samples({second})); // no duplicate sequence on retry
  assert(journal.recover_current().value->samples.size() == 2);
  assert(journal.recover_current().value->manifest.wall_ms == 2000);
  assert(journal.recover_current().value->manifest.active_ms == 2000);
  // Corrupt the newer manifest: select the older complete generation.
  const auto newest = journal.recover_current().value->manifest.generation;
  std::ofstream(root + "/journal/current/manifest." + std::to_string(newest % 2), std::ios::trunc) << "torn";
  const auto fallback = SessionJournal(root + "/journal").recover_current();
  assert(fallback && fallback.value->samples.size() == 1);
  std::ofstream(root + "/journal/current/manifest." + std::to_string((newest + 1) % 2), std::ios::trunc) << "torn";
  assert(!journal.recover_current()); assert(journal.has_current_session());

  SessionJournal published_sessions(root + "/published");
  assert(published_sessions.start(manifest));
  assert(published_sessions.append_samples({first}));
  auto published_manifest = manifest;
  published_manifest.complete = true;
  published_manifest.sample_count = 1;
  published_manifest.last_committed_sequence = 0;
  published_manifest.wall_ms = published_manifest.active_ms = published_manifest.plan_ms = 1000;
  assert(published_sessions.finalize(published_manifest));
  const auto session_ids = published_sessions.list_sessions();
  assert(session_ids.size() == 1 && session_ids.front() == manifest.uuid);

  // Cut every byte of a second sample chunk: recover only the first.
  SessionJournal cuts(root + "/cuts"); assert(cuts.start(manifest));
  assert(cuts.append_samples({first})); assert(cuts.append_samples({second}));
  const auto chunk = root + "/cuts/current/chunks/chunk-00000001.bin";
  std::ifstream chunk_in(chunk, std::ios::binary);
  const std::string original((std::istreambuf_iterator<char>(chunk_in)), {});
  for (std::size_t n = 0; n < original.size(); ++n) {
    { std::ofstream out(chunk, std::ios::binary | std::ios::trunc); out.write(original.data(), static_cast<std::streamsize>(n)); }
    const auto cut = SessionJournal(root + "/cuts").recover_current();
    assert(cut && cut.value->ignored_torn_tail && cut.value->samples.size() == 1);
  }
  FitInput input; input.start_utc_ms = manifest.start_utc_ms; input.time_trusted = true;
  input.wall_duration_ms = input.active_duration_ms = 1000; input.samples = {first};
  const auto bytes = encode_fit(input); assert(bytes);
  FitArtifactStore fits(root + "/fit");
  for (std::size_t budget : {0U, 20U}) {
    storage_test_write_budget = budget;
    assert(!fits.publish("partial", *bytes.value));
    storage_test_write_budget.reset();
    assert(!std::filesystem::exists(root + "/fit/partial.fit"));
  }
  std::ofstream(root + "/fit/partial.fit.tmp") << "power cut";
  const auto published = fits.publish("partial", *bytes.value); assert(published);
  assert(fits.publish("partial", *bytes.value));
  assert(fits.read(*published.value).value == bytes.value);

  WorkoutCache cache(root + "/cache");
  CachedWorkout cached; cached.event_id = "offline"; cached.athlete_id = "athlete";
  cached.local_date = "2026-09-17"; cached.timezone = "Europe/Berlin";
  cached.name = "Offline"; cached.ftp_used_for_preview = 250;
  cached.source_zwo = "<workout_file><workout><SteadyState Duration=\"10\" Power=\"0.5\"/></workout></workout_file>";
  cached.workout = *parse_zwo(cached.source_zwo).value;
  assert(cache.save(cached));
  auto changed = cached; changed.name = "Replacement";
  storage_test_write_budget = 15;
  assert(!cache.save(changed));
  storage_test_write_budget.reset();
  assert(WorkoutCache(root + "/cache").load("offline").value->name == "Offline");
  assert(cache.list_for_date("2026-09-17").size() == 1);
  auto stale = cached;
  stale.event_id = "stale";
  stale.name = "Stale";
  stale.workout.name = stale.name;
  assert(cache.save(stale));
  assert(cache.list_for_date("2026-09-17").size() == 2);
  storage_test_write_budget = 0;
  assert(!cache.replace_for_date("2026-09-17", {cached}));
  storage_test_write_budget.reset();
  assert(cache.list_for_date("2026-09-17").size() == 2);
  assert(cache.replace_for_date("2026-09-17", {cached}));
  assert(cache.list_for_date("2026-09-17").size() == 1);
  assert(!cache.load("stale"));
  assert(cache.replace_for_date("2026-09-17", {}));
  assert(cache.list_for_date("2026-09-17").empty());
  assert(!cache.replace_for_date("2026-09-18", {cached}));
  WorkoutCache empty_cache(root + "/empty-cache");
  assert(empty_cache.replace_for_date("2026-09-18", {}));
  assert(empty_cache.list_for_date("2026-09-18").empty());
  assert(!check_storage_capacity(root, UINT64_MAX).enough);

  UploadQueue queue(root + "/queue");
  UploadEntry entry; entry.session_uuid = "upload"; entry.fit_path = "upload.fit";
  entry.fit_sha256 = std::string(64, 'c'); entry.athlete_id = "athlete";
  assert(queue.enqueue(entry)); assert(queue.mark_fit_ready("upload"));
  assert(queue.mark_uploading("upload", 0));
  storage_test_write_budget = 10;
  assert(!queue.apply_result("upload", classify_upload_response(201, "{\"id\":\"remote\"}"), 1));
  storage_test_write_budget.reset();
  UploadQueue reboot(root + "/queue"); assert(reboot.load());
  assert(reboot.entries().size() == 1 && reboot.next_due(0, false));
  FakeTrainer trainer;
  SessionJournal finish_journal(root + "/finish");
  SessionCoordinator ride(&trainer, &finish_journal);
  assert(ride.enter_ready()); assert(ride.set_trainer_capabilities({true,true,true,true,true,0,500,5}));
  assert(ride.select_workout(make_workout(), ProfileSettings{}, false));
  assert(ride.start(1000, {1700000000000LL,true,3}, fresh_sensors(1000), true, "destination-athlete"));
  assert(ride.pause(PauseReason::kUser, 2000));
  storage_test_write_budget = 0;
  assert(!ride.end_and_save(3000));
  storage_test_write_budget.reset();
  assert(ride.snapshot().state == RideState::kFinishing);
  assert(ride.end_and_save(4000));
  assert(trainer.stop_count == 1);
  const auto finished_ride = finish_journal.read_session(ride.session_uuid());
  assert(finished_ride && finished_ride.value->manifest.destination_athlete_id == "destination-athlete");
  assert(ride.enter_ready());
  assert(ride.selected_workout() == nullptr);
  assert(ride.snapshot().clocks.wall_ms == 0);
  std::filesystem::remove_all(root);
}
