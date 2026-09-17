// Included after the shared deterministic fixtures in test_core.cpp.
void migration_contracts() {
  using namespace trainer_loop;
  const auto xml = [](const std::string& body) {
    return "<workout_file><workout>" + body + "</workout></workout_file>";
  };
  for (const std::string value : {"-1", "0", "nan", "inf", "1e308", "14401"}) {
    assert(!parse_zwo(xml("<SteadyState Duration=\"" + value + "\" Power=\"0.5\"/>")));
  }
  for (const std::string value : {"-1", "nan", "inf", "1e308", "100"}) {
    assert(!parse_zwo(xml("<SteadyState Duration=\"1\" Power=\"" + value + "\"/>")));
  }
  const auto intervals = parse_zwo(xml("<IntervalsT Repeat=\"2\" OnDuration=\"1\" OffDuration=\"2\" OnPower=\"1\" OffPower=\"0.5\"/>"));
  assert(intervals && intervals.value->segments.size() == 4);
  assert(resolved_target_range_at(*intervals.value, 999, 200).low == 200);
  assert(resolved_target_range_at(*intervals.value, 1000, 200).low == 100);
  assert(resolved_target_range_at(*intervals.value, 3000, 200).low == 200);
  assert(!parse_zwo(std::string(256U * 1024U + 1U, 'x')));
  assert(!parse_zwo(xml("<IntervalsT Repeat=\"1000000000\" OnDuration=\"1\" OffDuration=\"1\" OnPower=\"1\" OffPower=\"0.5\"/>")));
  const auto plan = parse_zwo(xml("<Ramp Duration=\"2\" PowerLow=\"0.5\" PowerHigh=\"1\"/><SteadyState Duration=\"1\" Power=\"0.25\"/>"));
  assert(plan);
  assert(resolved_target_range_at(*plan.value, 0, 200).low == 100);
  assert(resolved_target_range_at(*plan.value, 1000, 200).low == 150);
  assert(resolved_target_range_at(*plan.value, 1999, 200).low > 199);
  assert(resolved_target_range_at(*plan.value, 2000, 200).low == 50);
  assert(resolved_target_range_at(*plan.value, UINT32_MAX, 200).low == 50);

  assert(classify_upload_response(422, "Invalid FIT: duplicate field").classification == UploadHttpClass::kPermanent);
  assert(classify_upload_response(422, "not a duplicate activity").classification == UploadHttpClass::kPermanent);
  const auto root = temporary_directory();
  // Runtime represents timeout, DNS and TLS transport failures as status 0.
  for (const auto& scenario : std::vector<std::pair<std::string, int>>{
       {"401",401}, {"403",403}, {"408",408}, {"429",429}, {"500",500},
       {"503",503}, {"599",599}, {"timeout",0}, {"dns",0}, {"tls",0}, {"422",422}}) {
    const int code = scenario.second;
    UploadQueue queue(root + "/upload-" + scenario.first);
    UploadEntry entry;
    entry.session_uuid = "stable-id";
    entry.fit_path = "stable-id.fit";
    entry.fit_sha256 = std::string(64, 'a');
    entry.athlete_id = "original-athlete";
    entry.filename = "stable-id.fit";
    assert(queue.enqueue(entry));
    assert(queue.mark_fit_ready(entry.session_uuid));
    assert(!queue.next_due(0, true));
    assert(queue.mark_uploading(entry.session_uuid, 0));
    assert(queue.apply_result(entry.session_uuid, classify_upload_response(code, "invalid FIT", 60), 0));
    const auto state = queue.entries().front().state;
    assert(state == ((code == 401 || code == 403) ? UploadState::kAuthBlocked :
                      code == 422 ? UploadState::kPermanentError : UploadState::kRetryWait));
    assert(queue.entries().front().athlete_id == entry.athlete_id);
    if (code == 429) assert(queue.entries().front().next_attempt_ms == 60000);
  }
  // Acceptance with a lost response: durable UPLOADING becomes pending on boot,
  // retries the identical identity, and only an explicit duplicate completes it.
  UploadQueue queue(root + "/lost-response");
  UploadEntry entry;
  entry.session_uuid = "accepted"; entry.fit_path = "accepted.fit";
  entry.fit_sha256 = std::string(64, 'b'); entry.athlete_id = "athlete";
  entry.filename = "accepted.fit";
  assert(queue.enqueue(entry)); assert(queue.mark_fit_ready(entry.session_uuid));
  assert(queue.mark_uploading(entry.session_uuid, 0));
  UploadQueue reboot(root + "/lost-response"); assert(reboot.load());
  assert(reboot.next_due(0, false)); assert(reboot.enqueue(entry));
  assert(reboot.entries().size() == 1);
  assert(reboot.mark_uploading(entry.session_uuid, 0));
  assert(reboot.apply_result(entry.session_uuid, classify_upload_response(422, "DUPLICATE activity"), 1));
  assert(!reboot.next_due(UINT64_MAX, false));

  FakeTrainer trainer;
  SessionJournal journal(root + "/safety");
  SessionCoordinator ride(&trainer, &journal);
  assert(ride.enter_ready());
  assert(ride.select_workout(make_workout(), ProfileSettings{}, false));
  assert(!ride.start(1000, {1700000000000LL, true, 1}, fresh_sensors(1000), true));
  assert(trainer.acquire_count == 0);
  assert(ride.set_trainer_capabilities({true,true,true,true,true,0,500,5}));
  assert(!ride.start(1000, {1700000000000LL, false, 1}, fresh_sensors(1000), true));
  assert(!ride.start(1000, {1700000000000LL, true, 1}, fresh_sensors(1000), false));
  assert(trainer.acquire_count == 0);
  assert(ride.start(1000, {1700000000000LL, true, 1}, fresh_sensors(1000), true));
  assert(ride.pause(PauseReason::kUser, 2000));
  assert(ride.tick(5000, fresh_sensors(5000)));
  assert(ride.snapshot().state == RideState::kPaused);
  assert(ride.snapshot().clocks.wall_ms == 4000);
  assert(ride.snapshot().clocks.active_ms == 1000);
  assert(ride.snapshot().clocks.plan_ms == 1000);
  assert(trainer.acquire_count == 1);
  trainer.fail_commands = true;
  assert(!ride.resume(5000, fresh_sensors(5000)));
  assert(ride.snapshot().state == RideState::kPaused);
  trainer.fail_commands = false;
  assert(ride.resume(6000, fresh_sensors(6000)));
  auto disconnected = fresh_sensors(7000); disconnected.trainer_link = LinkState::kDisconnected;
  assert(ride.tick(7000, disconnected));
  assert(ride.snapshot().resistance_unknown);
  assert(ride.tick(8000, fresh_sensors(8000)));
  assert(ride.snapshot().state == RideState::kPaused);
  assert(ride.resume(8000, fresh_sensors(8000)));
  auto unsubscribed = fresh_sensors(9000); unsubscribed.telemetry_subscription = false;
  assert(ride.tick(9000, unsubscribed));
  assert(ride.snapshot().state == RideState::kPaused);
  assert(ride.end_and_save(9000, true));
  SessionCoordinator after_boot(&trainer, &journal);
  const unsigned acquisitions = trainer.acquire_count;
  assert(after_boot.enter_ready()); assert(after_boot.tick(10000, fresh_sensors(10000)));
  assert(after_boot.snapshot().state == RideState::kReady);
  assert(trainer.acquire_count == acquisitions);
  std::filesystem::remove_all(root);
}
