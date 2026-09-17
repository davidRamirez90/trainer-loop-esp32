#pragma once

#include "trainer_loop/intervals.h"
#include "trainer_loop/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trainer_loop {

enum class UploadState : std::uint8_t {
  kFitPending,
  kPending,
  kUploading,
  kUploaded,
  kRetryWait,
  kAuthBlocked,
  kPermanentError,
};

struct UploadEntry {
  std::string session_uuid;
  std::string fit_path;
  std::string fit_sha256;
  std::string athlete_id;
  std::string filename;
  UploadState state = UploadState::kFitPending;
  std::uint32_t attempts = 0U;
  std::uint64_t next_attempt_ms = 0U;
  int last_http_status = 0;
  std::string last_error_class;
  std::string remote_activity_id;
  std::uint64_t uploaded_at_ms = 0U;
};

class UploadQueue {
 public:
  explicit UploadQueue(std::string database_path);

  Status load();
  Status persist() const;
  Status retry_failed(std::uint64_t now_ms);
  Status enqueue(UploadEntry entry);
  Status mark_fit_ready(const std::string& session_uuid);
  Status mark_uploading(const std::string& session_uuid, std::uint64_t now_ms);
  Status apply_result(const std::string& session_uuid, const UploadHttpResult& result,
                      std::uint64_t now_ms);
  Status reassign_athlete(const std::string& session_uuid, const std::string& athlete_id);

  std::optional<UploadEntry> next_due(std::uint64_t now_ms, bool ride_running, bool network_available = true) const;
  const std::vector<UploadEntry>& entries() const { return entries_; }
  std::size_t pending_count() const;

 private:
  UploadEntry* find(const std::string& session_uuid);
  const UploadEntry* find(const std::string& session_uuid) const;

  std::string database_path_;
  std::vector<UploadEntry> entries_;
};

std::string upload_state_name(UploadState state);

}  // namespace trainer_loop
