void ui_settings_contracts() {
  using namespace trainer_loop;
  assert(ui_date_label("2026-09-18") == "18 Sep");
  assert(ui_date_label("not-a-date") == "not-a-date");
  assert(std::string(compact_link_state_label("Trainer disconnected")) == "OFF");
  assert(std::string(compact_link_state_label("Trainer connected")) == "OK");
  assert(std::string(compact_link_state_label("HR reconnecting")) == "RETRY");
  assert(UiCommandType::kRetry != UiCommandType::kRetryUploads);
  std::uint64_t monotonic = 1000;
  TimeService time([&] { return monotonic; });
  assert(time.set_from_sntp(1700000000000LL));
  monotonic = 6000;
  assert(time.snapshot().utc_ms == 1700000005000LL);
  SessionClocks ride_clock;
  assert(ride_clock.start(monotonic, {time.snapshot().utc_ms, true, 3}));
  assert(time.set_from_sntp(1700001000000LL));
  assert(ride_clock.utc_at(7000) == 1700000006000LL); // fixed ride anchor
  SetupSession setup;
  auto credentials = make_setup_credentials(42, 1000, 100);
  assert(setup.begin(credentials, "0123456789abcdef"));
  assert(setup.active(1099)); assert(!setup.active(1100));
  assert(!setup.authorize_request("192.168.4.1", "http://192.168.4.1", "0123456789abcdef", 1100, true));
  setup.close(); assert(setup.csrf_token().empty() && setup.credentials().passphrase.empty());
  SettingsStore store;
  const auto initial = store.prepare(ProfileSettings{}, {"ssid", "password", "athlete", "key", "admin"}, 0);
  assert(initial && store.commit(*initial.value, true, true, true));
  auto draft = store.current().profile;
  draft.ftp_watts = 275; draft.rider_kg = 77.5; draft.bike_kg = 9;
  draft.crr = 0.006; draft.cda = 0.35;
  SettingsGeneration disk = store.current();
  unsigned writes = 0;
  auto persist = [&](const SettingsGeneration& value) { disk = value; ++writes; return Status::ok(); };
  for (const auto state : {RideState::kRunning, RideState::kPaused, RideState::kFinishing}) {
    assert(!save_ui_profile(store, draft, state, persist));
  }
  assert(writes == 0);
  assert(!save_ui_profile(store, draft, RideState::kReady, [](const SettingsGeneration&) {
    return Status::failure(ErrorCode::kStorage, "injected NVS failure");
  }));
  assert(store.current().profile.ftp_watts == 250);
  assert(save_ui_profile(store, draft, RideState::kPreview, persist));
  SettingsStore reboot; assert(reboot.restore(disk));
  assert(reboot.current().profile.ftp_watts == 275);
  assert(reboot.current().profile.rider_kg == 77.5);
  assert(reboot.current().profile.bike_kg == 9);
  assert(reboot.current().profile.crr == 0.006);
  assert(reboot.current().profile.cda == 0.35);
  assert(reboot.current().profile.ftp_local_override && reboot.current().profile.weight_local_override);
  draft.ftp_watts = 601;
  assert(!save_ui_profile(store, draft, RideState::kReady, persist));
  assert(writes == 1);
  const auto root = temporary_directory();
  UploadQueue queue(root + "/queue");
  UploadEntry entry; entry.session_uuid = "retry"; entry.fit_path = "retry.fit";
  entry.fit_sha256 = std::string(64, 'a'); entry.athlete_id = "athlete";
  assert(queue.enqueue(entry)); assert(queue.mark_fit_ready("retry"));
  assert(queue.mark_uploading("retry", 0));
  assert(queue.apply_result("retry", classify_upload_response(429, "", 60), 0));
  assert(queue.retry_failed(1));
  assert(!queue.next_due(1, false)); // Retry-After remains binding.
  assert(queue.mark_uploading("retry", 60000));
  assert(queue.apply_result("retry", classify_upload_response(401, ""), 60000));
  assert(queue.retry_failed(60001));
  assert(queue.next_due(60001, false));
  assert(!queue.next_due(60001, false, false)); // offline worker must not send
  assert(queue.mark_uploading("retry", 60001));
  UploadQueue reboot_upload(root + "/queue"); assert(reboot_upload.load());
  assert(reboot_upload.next_due(0, false)); // new boot has a new monotonic epoch

  // Retry-After is a relative service deadline, not a stale monotonic value
  // from the previous boot. Rebooting must preserve the immutable FIT identity
  // and destination athlete while rebasing the wait into the new epoch.
  UploadQueue reboot_wait_source(root + "/retry-wait-reboot");
  UploadEntry retry_entry;
  retry_entry.session_uuid = "retry-wait-session";
  retry_entry.fit_path = "retry-wait-session.fit";
  retry_entry.fit_sha256 = std::string(64, 'd');
  retry_entry.athlete_id = "destination-athlete";
  retry_entry.filename = "retry-wait-session.fit";
  assert(reboot_wait_source.enqueue(retry_entry));
  assert(reboot_wait_source.mark_fit_ready(retry_entry.session_uuid));
  assert(reboot_wait_source.mark_uploading(retry_entry.session_uuid, 1000));
  assert(reboot_wait_source.apply_result(
      retry_entry.session_uuid, classify_upload_response(429, "", 60), 1000));
  UploadQueue reboot_wait(root + "/retry-wait-reboot");
  assert(reboot_wait.load(500));
  assert(reboot_wait.entries().front().fit_sha256 == retry_entry.fit_sha256);
  assert(reboot_wait.entries().front().athlete_id == retry_entry.athlete_id);
  assert(!reboot_wait.next_due(500, false));
  assert(reboot_wait.next_due(60500, false));
  assert(reboot_wait.mark_uploading(retry_entry.session_uuid, 60500));
  std::filesystem::remove_all(root);
}
