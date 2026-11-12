#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace trainer_loop {

struct JournalManifest {
  std::string uuid;
  std::string workout_id;
  std::string workout_name;
  std::uint32_t generation = 0U;
  std::uint32_t last_committed_sequence = 0U;
  std::uint32_t sample_count = 0U;
  std::uint32_t wall_ms = 0U;
  std::uint32_t active_ms = 0U;
  std::uint32_t plan_ms = 0U;
  bool complete = false;
  bool interrupted = false;
  std::int64_t start_utc_ms = 0;
  bool time_trusted = false;
  std::uint8_t time_quality = 0U;
  // Immutable upload destination captured at ride start. Empty is accepted
  // only for legacy manifests written before this field existed.
  std::string destination_athlete_id;
};

struct RecoveredSession {
  JournalManifest manifest;
  std::vector<TelemetrySample> samples;
  std::vector<SessionEvent> events;
  bool has_valid_prefix = false;
  bool ignored_torn_tail = false;
};

class SessionJournal {
 public:
  explicit SessionJournal(std::string root_directory);

  Status initialize();
  Status start(const JournalManifest& manifest);
  Status append_samples(const std::vector<TelemetrySample>& samples);
  Status append_event(const SessionEvent& event);
  Status update_manifest(JournalManifest manifest);
  Status flush();
  Status finalize(const JournalManifest& manifest);
  Status discard_current();

  bool has_current_session() const;
  Result<RecoveredSession> recover_current() const;
  Result<RecoveredSession> read_session(const std::string& uuid) const;
  std::vector<std::string> list_sessions() const;
  const std::string& root_directory() const { return root_directory_; }

 private:
  Status write_manifest(const JournalManifest& manifest) const;
  Result<JournalManifest> read_manifest() const;
  Result<JournalManifest> read_manifest_from(const std::string& directory) const;
  Result<RecoveredSession> recover_directory(const std::string& directory) const;
  std::string current_directory() const;

  std::string root_directory_;
};

struct StorageCapacity {
  std::uint64_t available_bytes = 0U;
  std::uint64_t required_bytes = 0U;
  bool enough = false;
};

StorageCapacity check_storage_capacity(const std::string& path, std::uint64_t required_bytes);

}  // namespace trainer_loop
