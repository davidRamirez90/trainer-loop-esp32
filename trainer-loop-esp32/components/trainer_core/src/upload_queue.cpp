#include "trainer_loop/storage_io.h"
#include "trainer_loop/upload_queue.h"

#include "trainer_loop/hash.h"
#include "trainer_loop/limits.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace trainer_loop {
namespace {

constexpr std::size_t kMaxQueueEntries = 64U;
constexpr std::size_t kMaxQueueLineBytes = 4096U;
constexpr std::uint64_t kRetryDelaysMs[] = {30000ULL, 120000ULL, 600000ULL, 1800000ULL, 7200000ULL};

char hex_digit(unsigned value) { return value < 10U ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10U); }

std::string encode_field(const std::string& value) {
  std::string encoded;
  encoded.reserve(value.size() * 2U);
  for (const char raw_byte : value) {
    const unsigned char byte = static_cast<unsigned char>(raw_byte);
    encoded.push_back(hex_digit(byte >> 4U));
    encoded.push_back(hex_digit(byte & 0x0fU));
  }
  return encoded;
}

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::optional<std::string> decode_field(const std::string& value, std::size_t maximum) {
  if ((value.size() % 2U) != 0U || value.size() / 2U > maximum) return std::nullopt;
  std::string decoded;
  decoded.reserve(value.size() / 2U);
  for (std::size_t i = 0U; i < value.size(); i += 2U) {
    const int high = hex_value(value[i]);
    const int low = hex_value(value[i + 1U]);
    if (high < 0 || low < 0) return std::nullopt;
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0U;
  while (start <= line.size()) {
    const std::size_t end = line.find('|', start);
    fields.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1U;
  }
  return fields;
}

std::optional<std::uint64_t> unsigned_number(const std::string& value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t number = 0U;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
  return number;
}

std::optional<int> signed_number(const std::string& value) {
  if (value.empty()) return std::nullopt;
  int number = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
  return number;
}

std::optional<UploadState> parse_state(const std::string& value) {
  if (value == "FIT_PENDING") return UploadState::kFitPending;
  if (value == "PENDING") return UploadState::kPending;
  if (value == "UPLOADING") return UploadState::kUploading;
  if (value == "UPLOADED") return UploadState::kUploaded;
  if (value == "RETRY_WAIT") return UploadState::kRetryWait;
  if (value == "AUTH_BLOCKED") return UploadState::kAuthBlocked;
  if (value == "PERMANENT_ERROR") return UploadState::kPermanentError;
  return std::nullopt;
}

bool valid_hash(const std::string& value) {
  if (value.size() != 64U) return false;
  for (const char character : value) {
    if (!std::isxdigit(static_cast<unsigned char>(character))) return false;
  }
  return true;
}

std::uint64_t retry_deadline(const UploadEntry& entry, std::uint64_t now_ms) {
  const std::size_t index = std::min<std::size_t>(entry.attempts, 4U);
  const std::uint64_t base = kRetryDelaysMs[index];
  const std::uint64_t jitter_window = std::min<std::uint64_t>(base / 10U, 120000U);
  const std::uint32_t entropy = fnv1a32(entry.session_uuid + std::to_string(entry.attempts));
  const std::int64_t jitter = jitter_window == 0U
                                  ? 0
                                  : static_cast<std::int64_t>(entropy % (2U * jitter_window + 1U)) -
                                        static_cast<std::int64_t>(jitter_window);
  const std::int64_t signed_delay = static_cast<std::int64_t>(base) + jitter;
  const std::uint64_t delay = signed_delay < 0 ? 0U : static_cast<std::uint64_t>(signed_delay);
  return now_ms > UINT64_MAX - delay ? UINT64_MAX : now_ms + delay;
}

std::uint64_t saturating_delay(std::uint64_t now_ms, std::uint64_t delay_ms) {
  return now_ms > UINT64_MAX - delay_ms ? UINT64_MAX : now_ms + delay_ms;
}

bool write_atomic(const std::string& path, const std::string& text) {
  const std::string temporary = path + ".tmp";
  std::FILE* file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) return false;
  const bool written = text.empty() || storage_write(text.data(), text.size(), file) == text.size();
  const bool flushed = std::fflush(file) == 0;
  const int descriptor = ::fileno(file);
  const bool synced = descriptor < 0 || ::fsync(descriptor) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!written || !flushed || !synced || !closed) {
    std::remove(temporary.c_str());
    return false;
  }
  return std::rename(temporary.c_str(), path.c_str()) == 0;
}

}  // namespace

std::string upload_state_name(UploadState state) {
  switch (state) {
    case UploadState::kFitPending: return "FIT_PENDING";
    case UploadState::kPending: return "PENDING";
    case UploadState::kUploading: return "UPLOADING";
    case UploadState::kUploaded: return "UPLOADED";
    case UploadState::kRetryWait: return "RETRY_WAIT";
    case UploadState::kAuthBlocked: return "AUTH_BLOCKED";
    case UploadState::kPermanentError: return "PERMANENT_ERROR";
  }
  return "PERMANENT_ERROR";
}

UploadQueue::UploadQueue(std::string database_path) : database_path_(std::move(database_path)) {}

Status UploadQueue::load(std::uint64_t monotonic_now_ms) {
  entries_.clear();
  std::ifstream input(database_path_);
  if (!input) return Status::ok();  // First boot has an empty queue.
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    if (line.size() > kMaxQueueLineBytes) {
      return Status::failure(ErrorCode::kBounds, "upload queue record is too large");
    }
    const auto fields = split(line);
    if ((fields.size() != 12U && fields.size() != 13U) || entries_.size() >= kMaxQueueEntries) {
      return Status::failure(ErrorCode::kStorage, "upload queue record is invalid");
    }
    std::vector<std::string> decoded;
    decoded.reserve(7U);
    static constexpr std::size_t kFieldMaximums[] = {96U, 256U, 64U, kMaxAthleteIdBytes, 128U, 64U, 96U};
    for (unsigned i = 0U; i < 7U; ++i) {
      const auto value = decode_field(fields[i], kFieldMaximums[i]);
      if (!value) return Status::failure(ErrorCode::kStorage, "upload queue text is corrupt");
      decoded.push_back(*value);
    }
    const auto attempts = unsigned_number(fields[7]);
    const auto next = unsigned_number(fields[8]);
    const auto status = signed_number(fields[9]);
    const auto uploaded = unsigned_number(fields[11]);
    const auto retry_delay = fields.size() == 13U ? unsigned_number(fields[12]) : std::optional<std::uint64_t>(0U);
    if (!attempts || !next || !status || !uploaded || !retry_delay || *attempts > UINT32_MAX || *status < 0 || *status > 999) {
      return Status::failure(ErrorCode::kStorage, "upload queue number is invalid");
    }
    const auto state = parse_state(fields[10]);
    if (!state || decoded[0].empty() || decoded[0].size() > 96U || decoded[1].empty() || decoded[1].size() > 256U ||
        !valid_hash(decoded[2]) || decoded[3].empty() || decoded[3].size() > kMaxAthleteIdBytes ||
        decoded[4].size() > 128U || decoded[5].size() > 64U || decoded[6].size() > 96U) {
      return Status::failure(ErrorCode::kStorage, "upload queue identity is invalid");
    }
    UploadEntry entry;
    entry.session_uuid = decoded[0];
    entry.fit_path = decoded[1];
    entry.fit_sha256 = decoded[2];
    entry.athlete_id = decoded[3];
    entry.filename = decoded[4];
    entry.last_error_class = decoded[5];
    entry.remote_activity_id = decoded[6];
    entry.state = *state;
    entry.attempts = static_cast<std::uint32_t>(*attempts);
    entry.next_attempt_ms = *next;
    entry.retry_delay_ms = *retry_delay;
    entry.last_http_status = *status;
    entry.uploaded_at_ms = *uploaded;
    if (entry.state == UploadState::kUploading) {
      entry.state = UploadState::kPending;
      entry.next_attempt_ms = 0U;
      entry.retry_delay_ms = 0U;
    } else if (entry.state == UploadState::kRetryWait) {
      // The persisted absolute value belongs to the previous monotonic epoch.
      // Reconstruct the deadline from the durable relative wait instead.
      entry.next_attempt_ms = saturating_delay(monotonic_now_ms, entry.retry_delay_ms);
    }
    entries_.push_back(std::move(entry));
  }
  return Status::ok();
}

Status UploadQueue::retry_failed(std::uint64_t now_ms) {
  const auto previous = entries_;
  for (auto& entry : entries_) {
    if (entry.state == UploadState::kAuthBlocked ||
        (entry.state == UploadState::kRetryWait && entry.last_error_class != "rate_limited")) {
      entry.state = UploadState::kPending;
      entry.next_attempt_ms = now_ms;
      entry.retry_delay_ms = 0U;
    }
  }
  const Status saved = persist();
  if (!saved) entries_ = previous;
  return saved;
}

Status UploadQueue::persist() const {
  std::ostringstream output;
  for (const auto& entry : entries_) {
    output << encode_field(entry.session_uuid) << '|'
           << encode_field(entry.fit_path) << '|'
           << encode_field(entry.fit_sha256) << '|'
           << encode_field(entry.athlete_id) << '|'
           << encode_field(entry.filename) << '|'
           << encode_field(entry.last_error_class) << '|'
           << encode_field(entry.remote_activity_id) << '|'
           << entry.attempts << '|' << entry.next_attempt_ms << '|'
           << entry.last_http_status << '|' << upload_state_name(entry.state) << '|' << entry.uploaded_at_ms << '|'
           << entry.retry_delay_ms << '\n';
  }
  if (!output) return Status::failure(ErrorCode::kStorage, "cannot write upload queue");
  if (!write_atomic(database_path_, output.str())) {
    return Status::failure(ErrorCode::kStorage, "cannot publish upload queue");
  }
  return Status::ok();
}

UploadEntry* UploadQueue::find(const std::string& session_uuid) {
  for (auto& entry : entries_) if (entry.session_uuid == session_uuid) return &entry;
  return nullptr;
}

const UploadEntry* UploadQueue::find(const std::string& session_uuid) const {
  for (const auto& entry : entries_) if (entry.session_uuid == session_uuid) return &entry;
  return nullptr;
}

Status UploadQueue::enqueue(UploadEntry entry) {
  if (entry.session_uuid.empty() || entry.session_uuid.size() > 96U || entry.fit_path.empty() || entry.fit_path.size() > 256U ||
      !valid_hash(entry.fit_sha256) || entry.athlete_id.empty() || entry.athlete_id.size() > kMaxAthleteIdBytes ||
      entry.filename.size() > 128U || entry.last_error_class.size() > 64U || entry.remote_activity_id.size() > 96U ||
      entry.last_http_status < 0 || entry.last_http_status > 999) {
    return Status::failure(ErrorCode::kInvalidArgument, "upload entry identity is incomplete");
  }
  if (UploadEntry* existing = find(entry.session_uuid)) {
    if (existing->fit_sha256 != entry.fit_sha256) return Status::failure(ErrorCode::kConflict, "session UUID has a different FIT hash");
    return Status::ok();
  }
  if (entries_.size() >= kMaxQueueEntries) return Status::failure(ErrorCode::kBounds, "upload queue is full");
  entry.state = UploadState::kFitPending;
  entries_.push_back(std::move(entry));
  const Status persisted = persist();
  if (!persisted) entries_.pop_back();
  return persisted;
}

Status UploadQueue::mark_fit_ready(const std::string& session_uuid) {
  UploadEntry* entry = find(session_uuid);
  if (entry == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "upload session is unknown");
  if (entry->state != UploadState::kFitPending) return Status::failure(ErrorCode::kConflict, "FIT is already finalized");
  const UploadEntry previous = *entry;
  entry->state = UploadState::kPending;
  entry->next_attempt_ms = 0U;
  entry->retry_delay_ms = 0U;
  const Status persisted = persist();
  if (!persisted) *entry = previous;
  return persisted;
}

Status UploadQueue::mark_uploading(const std::string& session_uuid, std::uint64_t now_ms) {
  UploadEntry* entry = find(session_uuid);
  if (entry == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "upload session is unknown");
  if (entry->state != UploadState::kPending && entry->state != UploadState::kRetryWait) {
    return Status::failure(ErrorCode::kConflict, "upload is not due");
  }
  if (entry->next_attempt_ms > now_ms) return Status::failure(ErrorCode::kConflict, "upload retry is not due");
  const UploadEntry previous = *entry;
  entry->state = UploadState::kUploading;
  entry->retry_delay_ms = 0U;
  const Status persisted = persist();
  if (!persisted) *entry = previous;
  return persisted;
}

Status UploadQueue::apply_result(const std::string& session_uuid, const UploadHttpResult& result,
                                 std::uint64_t now_ms) {
  UploadEntry* entry = find(session_uuid);
  if (entry == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "upload session is unknown");
  if (entry->state != UploadState::kUploading) return Status::failure(ErrorCode::kConflict, "upload is not in flight");
  if (result.status < 0 || result.status > 999 || result.remote_activity_id.size() > 96U) {
    return Status::failure(ErrorCode::kBounds, "upload response is invalid");
  }
  const UploadEntry previous = *entry;
  entry->last_http_status = result.status;
  entry->remote_activity_id = result.remote_activity_id;
  entry->next_attempt_ms = 0U;
  entry->retry_delay_ms = 0U;
  switch (result.classification) {
    case UploadHttpClass::kAccepted:
    case UploadHttpClass::kDuplicate:
      entry->state = UploadState::kUploaded;
      entry->uploaded_at_ms = now_ms;
      entry->last_error_class = result.classification == UploadHttpClass::kDuplicate ? "duplicate" : "";
      break;
    case UploadHttpClass::kAuthentication:
      entry->state = UploadState::kAuthBlocked;
      entry->last_error_class = "authentication";
      break;
    case UploadHttpClass::kRateLimited:
      entry->state = UploadState::kRetryWait;
      entry->last_error_class = "rate_limited";
      entry->next_attempt_ms = result.retry_after_s == 0U
                                   ? retry_deadline(*entry, now_ms)
                                   : saturating_delay(now_ms, static_cast<std::uint64_t>(result.retry_after_s) * 1000ULL);
      entry->retry_delay_ms = entry->next_attempt_ms >= now_ms ? entry->next_attempt_ms - now_ms : 0U;
      if (entry->attempts < UINT32_MAX) ++entry->attempts;
      break;
    case UploadHttpClass::kTransient:
      entry->state = UploadState::kRetryWait;
      entry->last_error_class = "transient";
      entry->next_attempt_ms = retry_deadline(*entry, now_ms);
      entry->retry_delay_ms = entry->next_attempt_ms >= now_ms ? entry->next_attempt_ms - now_ms : 0U;
      if (entry->attempts < UINT32_MAX) ++entry->attempts;
      break;
    case UploadHttpClass::kPermanent:
      entry->state = UploadState::kPermanentError;
      entry->last_error_class = "permanent";
      break;
  }
  const Status persisted = persist();
  if (!persisted) *entry = previous;
  return persisted;
}

Status UploadQueue::reassign_athlete(const std::string& session_uuid, const std::string& athlete_id) {
  if (athlete_id.empty() || athlete_id.size() > kMaxAthleteIdBytes) return Status::failure(ErrorCode::kBounds, "athlete ID is invalid");
  UploadEntry* entry = find(session_uuid);
  if (entry == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "upload session is unknown");
  if (entry->state == UploadState::kUploaded) return Status::failure(ErrorCode::kConflict, "uploaded session cannot be reassigned");
  const UploadEntry previous = *entry;
  entry->athlete_id = athlete_id;
  entry->state = UploadState::kPending;
  entry->attempts = 0U;
  entry->last_error_class.clear();
  entry->next_attempt_ms = 0U;
  entry->retry_delay_ms = 0U;
  const Status persisted = persist();
  if (!persisted) *entry = previous;
  return persisted;
}

std::optional<UploadEntry> UploadQueue::next_due(std::uint64_t now_ms, bool ride_running, bool network_available) const {
  if (ride_running || !network_available) return std::nullopt;
  for (const auto& entry : entries_) {
    if ((entry.state == UploadState::kPending || entry.state == UploadState::kRetryWait) && entry.next_attempt_ms <= now_ms) {
      return entry;
    }
  }
  return std::nullopt;
}

std::size_t UploadQueue::pending_count() const {
  std::size_t count = 0U;
  for (const auto& entry : entries_) {
    if (entry.state != UploadState::kUploaded) ++count;
  }
  return count;
}

}  // namespace trainer_loop
