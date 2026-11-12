#include "trainer_loop/storage_io.h"
#include "trainer_loop/journal.h"

#include "trainer_loop/limits.h"
#include "trainer_loop/telemetry.h"
#include "trainer_loop/time_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_littlefs.h"
#else
#include <sys/statvfs.h>
#endif
#include <unistd.h>

namespace trainer_loop {
namespace {

constexpr char kManifestMagic[] = "TLMF";
constexpr char kChunkMagic[] = "TLCH";
constexpr std::uint16_t kManifestVersion = 3U;
constexpr std::uint16_t kChunkVersion = 2U;
constexpr std::size_t kManifestMaxBytes = 4096U;
constexpr std::size_t kChunkHeaderBytes = 20U;
constexpr std::size_t kMaxJournalEventBytes = 16U * 1024U;
constexpr std::uint32_t kMaxChunkSequence = 99999999U;

void put_u16(std::vector<std::uint8_t>* data, std::uint16_t value) {
  data->push_back(static_cast<std::uint8_t>(value & 0xffU));
  data->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void put_u32(std::vector<std::uint8_t>* data, std::uint32_t value) {
  for (unsigned i = 0U; i < 4U; ++i) data->push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
}

void put_u64(std::vector<std::uint8_t>* data, std::uint64_t value) {
  for (unsigned i = 0U; i < 8U; ++i) data->push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
}

void put_string(std::vector<std::uint8_t>* data, const std::string& value, std::size_t max_size) {
  const std::size_t length = std::min(value.size(), max_size);
  put_u16(data, static_cast<std::uint16_t>(length));
  data->insert(data->end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
}

std::uint16_t get_u16(const std::vector<std::uint8_t>& data, std::size_t* offset) {
  const std::uint16_t value = static_cast<std::uint16_t>(data[*offset]) |
                              static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[*offset + 1U]) << 8U);
  *offset += 2U;
  return value;
}

std::uint32_t get_u32(const std::vector<std::uint8_t>& data, std::size_t* offset) {
  std::uint32_t value = 0U;
  for (unsigned i = 0U; i < 4U; ++i) value |= static_cast<std::uint32_t>(data[*offset + i]) << (8U * i);
  *offset += 4U;
  return value;
}

std::uint64_t get_u64(const std::vector<std::uint8_t>& data, std::size_t* offset) {
  std::uint64_t value = 0U;
  for (unsigned i = 0U; i < 8U; ++i) value |= static_cast<std::uint64_t>(data[*offset + i]) << (8U * i);
  *offset += 8U;
  return value;
}

bool ensure_directory(const std::string& path) {
  if (::mkdir(path.c_str(), 0700) == 0) return true;
  return errno == EEXIST;
}

bool ensure_tree(const std::string& root) {
  std::string partial;
  for (std::size_t i = 0U; i < root.size(); ++i) {
    partial.push_back(root[i]);
    if (root[i] == '/') {
      if (!partial.empty() && partial != "/" && !ensure_directory(partial)) return false;
    }
  }
  return ensure_directory(root);
}

bool remove_tree(const std::string& path) {
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0) return errno == ENOENT;
  if (!S_ISDIR(status.st_mode)) {
    return ::remove(path.c_str()) == 0 || errno == ENOENT;
  }
  DIR* handle = ::opendir(path.c_str());
  if (handle == nullptr) return errno == ENOENT;
  bool removed = true;
  while (const dirent* entry = ::readdir(handle)) {
    const std::string name(entry->d_name);
    if (name == "." || name == "..") continue;
    if (!remove_tree(path + "/" + name)) {
      removed = false;
      break;
    }
  }
  const int close_result = ::closedir(handle);
  if (!removed || close_result != 0) return false;
  return ::rmdir(path.c_str()) == 0 || errno == ENOENT;
}

bool valid_component(const std::string& value) {
  if (value.empty() || value.size() > 96U) return false;
  for (const char character : value) {
    if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '-' || character == '_')) return false;
  }
  return true;
}

bool write_atomic(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const std::string temporary = path + ".tmp";
  std::FILE* file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) return false;
  const bool written = bytes.empty() || storage_write(bytes.data(), bytes.size(), file) == bytes.size();
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

Result<std::vector<std::uint8_t>> read_bytes(const std::string& path, std::size_t maximum) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kStorage, "cannot open journal record");
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > maximum) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kBounds, "journal record is too large");
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input && !bytes.empty()) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kStorage, "journal read failed");
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

std::vector<std::string> chunk_names(const std::string& directory) {
  std::vector<std::string> names;
  DIR* handle = ::opendir(directory.c_str());
  if (handle == nullptr) return names;
  while (const dirent* entry = ::readdir(handle)) {
    const std::string name(entry->d_name);
    if (name.size() == 18U && name.rfind("chunk-", 0U) == 0U && name.substr(14U) == ".bin") names.push_back(name);
  }
  ::closedir(handle);
  std::sort(names.begin(), names.end());
  return names;
}

std::string chunk_file_name(std::uint32_t sequence) {
  std::ostringstream stream;
  stream << "chunk-" << std::setfill('0') << std::setw(8) << sequence << ".bin";
  return stream.str();
}

}  // namespace

SessionJournal::SessionJournal(std::string root_directory)
    : root_directory_(std::move(root_directory)) {}

std::string SessionJournal::current_directory() const { return root_directory_ + "/current"; }

Status SessionJournal::initialize() {
  if (root_directory_.empty() || !ensure_tree(root_directory_) || !ensure_directory(root_directory_ + "/sessions")) {
    return Status::failure(ErrorCode::kStorage, "cannot create journal directories");
  }
  return Status::ok();
}

Status SessionJournal::start(const JournalManifest& manifest) {
  if (!valid_component(manifest.uuid)) return Status::failure(ErrorCode::kInvalidArgument, "invalid session UUID");
  if (!manifest.workout_id.empty() && !valid_component(manifest.workout_id)) {
    return Status::failure(ErrorCode::kInvalidArgument, "invalid workout ID");
  }
  if (manifest.workout_name.empty() || manifest.workout_name.size() > kMaxWorkoutNameBytes ||
      manifest.complete || manifest.interrupted) {
    return Status::failure(ErrorCode::kInvalidArgument, "invalid initial journal manifest");
  }
  const Status initialized = initialize();
  if (!initialized) return initialized;
  struct stat current_status {};
  if (::stat(current_directory().c_str(), &current_status) == 0) {
    return Status::failure(ErrorCode::kConflict, "an interrupted session already exists");
  }
  if (!ensure_directory(current_directory()) || !ensure_directory(current_directory() + "/chunks")) {
    return Status::failure(ErrorCode::kStorage, "cannot create current journal");
  }
  JournalManifest initial = manifest;
  initial.generation = 1U;
  initial.last_committed_sequence = 0U;
  initial.sample_count = 0U;
  initial.wall_ms = 0U;
  initial.active_ms = 0U;
  initial.plan_ms = 0U;
  if (!write_manifest(initial)) return Status::failure(ErrorCode::kStorage, "cannot write journal manifest");
  return Status::ok();
}

Status SessionJournal::write_manifest(const JournalManifest& manifest) const {
  if (!valid_component(manifest.uuid) ||
      (!manifest.workout_id.empty() && !valid_component(manifest.workout_id)) ||
      manifest.workout_name.empty() || manifest.workout_name.size() > kMaxWorkoutNameBytes ||
      manifest.destination_athlete_id.size() > kMaxAthleteIdBytes ||
      (manifest.complete && manifest.interrupted) ||
      (manifest.time_trusted && !plausible_utc_ms(manifest.start_utc_ms)) ||
      (manifest.sample_count == 0U && manifest.last_committed_sequence != 0U)) {
    return Status::failure(ErrorCode::kInvalidArgument, "journal manifest is invalid");
  }
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), kManifestMagic, kManifestMagic + 4);
  put_u16(&bytes, kManifestVersion);
  put_u32(&bytes, manifest.generation);
  bytes.push_back(manifest.complete ? 1U : 0U);
  bytes.push_back(manifest.interrupted ? 1U : 0U);
  put_u64(&bytes, static_cast<std::uint64_t>(manifest.start_utc_ms));
  bytes.push_back(manifest.time_trusted ? 1U : 0U);
  bytes.push_back(manifest.time_quality);
  put_u32(&bytes, manifest.last_committed_sequence);
  put_u32(&bytes, manifest.sample_count);
  put_u32(&bytes, manifest.wall_ms);
  put_u32(&bytes, manifest.active_ms);
  put_u32(&bytes, manifest.plan_ms);
  put_string(&bytes, manifest.uuid, 96U);
  put_string(&bytes, manifest.workout_id, 96U);
  put_string(&bytes, manifest.workout_name, kMaxWorkoutNameBytes);
  put_string(&bytes, manifest.destination_athlete_id, kMaxAthleteIdBytes);
  put_u32(&bytes, crc32(bytes));
  const std::string path = current_directory() + "/manifest." + std::to_string(manifest.generation % 2U);
  return write_atomic(path, bytes) ? Status::ok() : Status::failure(ErrorCode::kStorage, "manifest commit failed");
}

Result<JournalManifest> SessionJournal::read_manifest() const {
  return read_manifest_from(current_directory());
}

Result<JournalManifest> SessionJournal::read_manifest_from(const std::string& directory) const {
  Result<JournalManifest> best = Result<JournalManifest>::failure(ErrorCode::kStorage, "no valid manifest");
  for (unsigned slot = 0U; slot < 2U; ++slot) {
    const auto bytes_result = read_bytes(directory + "/manifest." + std::to_string(slot), kManifestMaxBytes);
    if (!bytes_result || bytes_result.value->size() < 4U + 2U + 4U + 2U + 8U + 2U + 5U * 4U + 6U) continue;
    const auto& bytes = *bytes_result.value;
    if (!std::equal(bytes.begin(), bytes.begin() + 4, kManifestMagic)) continue;
    std::size_t offset = 4U;
    const std::uint16_t version = get_u16(bytes, &offset);
    if (version != 2U && version != kManifestVersion) continue;
    const std::uint32_t generation = get_u32(bytes, &offset);
    const std::uint8_t complete_raw = bytes[offset++];
    const std::uint8_t interrupted_raw = bytes[offset++];
    if (complete_raw > 1U || interrupted_raw > 1U || (complete_raw != 0U && interrupted_raw != 0U)) continue;
    const bool complete = complete_raw != 0U;
    const bool interrupted = interrupted_raw != 0U;
    const std::int64_t start_utc_ms = static_cast<std::int64_t>(get_u64(bytes, &offset));
    const std::uint8_t time_trusted_raw = bytes[offset++];
    const std::uint8_t time_quality = bytes[offset++];
    if (time_trusted_raw > 1U || (time_trusted_raw != 0U && !plausible_utc_ms(start_utc_ms))) continue;
    const std::uint32_t last_seq = get_u32(bytes, &offset);
    const std::uint32_t sample_count = get_u32(bytes, &offset);
    const std::uint32_t wall_ms = get_u32(bytes, &offset);
    const std::uint32_t active_ms = get_u32(bytes, &offset);
    const std::uint32_t plan_ms = get_u32(bytes, &offset);
    auto read_string = [&](std::string* target) -> bool {
      if (offset + 2U > bytes.size()) return false;
      const std::size_t length = get_u16(bytes, &offset);
      if (offset + length > bytes.size()) return false;
      *target = std::string(reinterpret_cast<const char*>(bytes.data() + offset), length);
      offset += length;
      return true;
    };
    JournalManifest manifest;
    manifest.generation = generation;
    manifest.complete = complete;
    manifest.interrupted = interrupted;
    manifest.start_utc_ms = start_utc_ms;
    manifest.time_trusted = time_trusted_raw != 0U;
    manifest.time_quality = time_quality;
    manifest.last_committed_sequence = last_seq;
    manifest.sample_count = sample_count;
    manifest.wall_ms = wall_ms;
    manifest.active_ms = active_ms;
    manifest.plan_ms = plan_ms;
    if (!read_string(&manifest.uuid) || !read_string(&manifest.workout_id) || !read_string(&manifest.workout_name) ||
        (version >= kManifestVersion && !read_string(&manifest.destination_athlete_id)) ||
        offset + 4U != bytes.size()) continue;
    const std::uint32_t stored_crc = get_u32(bytes, &offset);
    if (stored_crc != crc32(bytes.data(), bytes.size() - 4U)) continue;
    if (!valid_component(manifest.uuid) ||
        (!manifest.workout_id.empty() && !valid_component(manifest.workout_id)) ||
        manifest.workout_name.empty() || manifest.workout_name.size() > kMaxWorkoutNameBytes ||
        manifest.destination_athlete_id.size() > kMaxAthleteIdBytes ||
        (manifest.time_trusted && !plausible_utc_ms(manifest.start_utc_ms)) ||
        (sample_count == 0U && last_seq != 0U) || sample_count > kMaxSessionSamples) continue;
    if (!best.value.has_value() || generation > best.value->generation) best = Result<JournalManifest>::success(std::move(manifest));
  }
  return best;
}

Status SessionJournal::append_samples(const std::vector<TelemetrySample>& samples) {
  if (samples.empty()) return Status::ok();
  const auto manifest_result = read_manifest();
  if (!manifest_result) return Status::failure(ErrorCode::kStorage, "cannot append without a valid manifest");
  const std::uint32_t expected_first = manifest_result.value->sample_count == 0U
                                           ? samples.front().sequence
                                           : manifest_result.value->last_committed_sequence + 1U;
  if (samples.front().sequence != expected_first) {
    return Status::failure(ErrorCode::kInvalidArgument, "sample sequence does not continue the journal");
  }
  if (samples.size() > static_cast<std::size_t>(UINT32_MAX) ||
      samples.size() > kMaxSessionSamples ||
      manifest_result.value->sample_count > kMaxSessionSamples - samples.size() ||
      (manifest_result.value->sample_count != 0U && manifest_result.value->last_committed_sequence == UINT32_MAX)) {
    return Status::failure(ErrorCode::kOverflow, "journal sample count overflows");
  }
  if (manifest_result.value->generation == UINT32_MAX) {
    return Status::failure(ErrorCode::kOverflow, "journal generation overflows");
  }
  for (std::size_t i = 0U; i < samples.size(); ++i) {
    if (expected_first > UINT32_MAX - static_cast<std::uint32_t>(i) ||
        expected_first + static_cast<std::uint32_t>(i) > kMaxChunkSequence ||
        samples[i].sequence != expected_first + static_cast<std::uint32_t>(i)) {
      return Status::failure(ErrorCode::kInvalidArgument, "sample sequence is not contiguous");
    }
  }
  std::uint32_t first_sequence = expected_first;
  std::size_t position = 0U;
  while (position < samples.size()) {
    const std::size_t max_samples = (kMaxJournalChunkBytes - kChunkHeaderBytes) / kTelemetrySampleBytes;
    const std::size_t count = std::min(max_samples, samples.size() - position);
    // The complete vector was sequence-validated before the first chunk was
    // written, so a later chunk cannot leave a valid-looking orphan caused by
    // an input gap or sequence overflow.
    std::vector<std::uint8_t> payload;
    payload.reserve(count * kTelemetrySampleBytes);
    for (std::size_t i = 0U; i < count; ++i) {
      const auto encoded = serialize_sample(samples[position + i]);
      payload.insert(payload.end(), encoded.begin(), encoded.end());
    }
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), kChunkMagic, kChunkMagic + 4);
    put_u16(&bytes, kChunkVersion);
    put_u32(&bytes, first_sequence);
    put_u16(&bytes, static_cast<std::uint16_t>(count));
    put_u32(&bytes, static_cast<std::uint32_t>(payload.size()));
    put_u32(&bytes, crc32(payload));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const std::string name = current_directory() + "/chunks/" + chunk_file_name(first_sequence);
    if (!write_atomic(name, bytes)) return Status::failure(ErrorCode::kStorage, "sample chunk commit failed");
    position += count;
    if (position < samples.size()) {
      if (first_sequence > UINT32_MAX - static_cast<std::uint32_t>(count)) {
        return Status::failure(ErrorCode::kOverflow, "journal sample sequence overflows");
      }
      first_sequence += static_cast<std::uint32_t>(count);
    }
  }
  JournalManifest next = *manifest_result.value;
  if (next.generation == UINT32_MAX) return Status::failure(ErrorCode::kOverflow, "journal generation overflows");
  next.generation += 1U;
  next.last_committed_sequence = samples.back().sequence;
  next.sample_count += static_cast<std::uint32_t>(samples.size());
  if (!write_manifest(next)) return Status::failure(ErrorCode::kStorage, "sample manifest commit failed");
  return Status::ok();
}

Status SessionJournal::append_event(const SessionEvent& event) {
  if (event.reason.size() > kMaxEventReasonBytes) return Status::failure(ErrorCode::kBounds, "event reason is too long");
  if (static_cast<unsigned>(event.type) > static_cast<unsigned>(SessionEvent::Type::kRecovery)) {
    return Status::failure(ErrorCode::kInvalidArgument, "event type is invalid");
  }
  const auto manifest_result = read_manifest();
  if (!manifest_result) return Status::failure(ErrorCode::kStorage, "cannot append event without manifest");
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'E', 'V', 'T', '1'});
  bytes.push_back(static_cast<std::uint8_t>(event.type));
  put_u32(&bytes, event.wall_ms);
  put_u32(&bytes, event.active_ms);
  put_u32(&bytes, event.plan_ms);
  bytes.push_back(static_cast<std::uint8_t>(event.reason.size()));
  bytes.insert(bytes.end(), event.reason.begin(), event.reason.end());
  put_u32(&bytes, crc32(bytes));
  const std::string path = current_directory() + "/events.bin";
  struct stat existing {};
  if (::stat(path.c_str(), &existing) == 0 &&
      (existing.st_size < 0 || static_cast<std::uint64_t>(existing.st_size) + bytes.size() > kMaxJournalEventBytes)) {
    return Status::failure(ErrorCode::kBounds, "journal event log is full");
  }
  // Events are small and bounded: atomically publish the complete event log
  // so a failed append cannot poison subsequent pause/resume records.
  if (existing.st_size > 0) {
    auto prior = read_bytes(path, kMaxJournalEventBytes);
    if (!prior) return Status::failure(prior.error->code, prior.error->message);
    prior.value->insert(prior.value->end(), bytes.begin(), bytes.end());
    bytes = std::move(*prior.value);
  }
  if (!write_atomic(path, bytes)) return Status::failure(ErrorCode::kStorage, "event commit failed");
  return Status::ok();
}

Status SessionJournal::update_manifest(JournalManifest manifest) {
  const auto current = read_manifest();
  if (!current) return Status::failure(ErrorCode::kStorage, "cannot update missing manifest");
  if (manifest.uuid != current.value->uuid) return Status::failure(ErrorCode::kConflict, "manifest UUID changed");
  if (current.value->generation == UINT32_MAX) return Status::failure(ErrorCode::kOverflow, "journal generation overflows");
  manifest.generation = current.value->generation + 1U;
  manifest.last_committed_sequence = current.value->last_committed_sequence;
  manifest.sample_count = current.value->sample_count;
  return write_manifest(manifest);
}

Status SessionJournal::flush() {
  // Chunks are committed atomically by append_samples. This hook exists so
  // the coordinator can make a transition flush explicit and so an ESP VFS
  // implementation can add fsync policy without changing the domain API.
  const auto manifest = read_manifest();
  return manifest ? Status::ok() : Status::failure(ErrorCode::kStorage, "journal has no committed manifest");
}

Status SessionJournal::finalize(const JournalManifest& manifest) {
  const Status updated = update_manifest(manifest);
  if (!updated) return updated;
  const auto current = read_manifest();
  if (!current) return Status::failure(ErrorCode::kStorage, "cannot finalize missing manifest");
  const std::string destination = root_directory_ + "/sessions/" + current.value->uuid;
  if (::rename(current_directory().c_str(), destination.c_str()) != 0) {
    return Status::failure(ErrorCode::kStorage, "cannot publish completed session");
  }
  return Status::ok();
}

Status SessionJournal::discard_current() {
  // The current directory is the one explicitly owned by the active ride.
  // Discarding it removes the recoverable journal without touching finalized
  // sessions, history, or upload artifacts from earlier rides.
  if (!remove_tree(current_directory())) {
    return Status::failure(ErrorCode::kStorage, "cannot discard current session journal");
  }
  return Status::ok();
}

bool SessionJournal::has_current_session() const {
  struct stat current_status {};
  return ::stat(current_directory().c_str(), &current_status) == 0 &&
         S_ISDIR(current_status.st_mode);
}

Result<RecoveredSession> SessionJournal::recover_current() const {
  return recover_directory(current_directory());
}

Result<RecoveredSession> SessionJournal::read_session(const std::string& uuid) const {
  if (!valid_component(uuid)) {
    return Result<RecoveredSession>::failure(ErrorCode::kInvalidArgument, "invalid session UUID");
  }
  return recover_directory(root_directory_ + "/sessions/" + uuid);
}

std::vector<std::string> SessionJournal::list_sessions() const {
  std::vector<std::string> sessions;
  DIR* handle = ::opendir((root_directory_ + "/sessions").c_str());
  if (handle == nullptr) return sessions;
  while (const dirent* entry = ::readdir(handle)) {
    const std::string uuid(entry->d_name);
    if (!valid_component(uuid)) continue;
    struct stat status {};
    if (::stat((root_directory_ + "/sessions/" + uuid).c_str(), &status) == 0 && S_ISDIR(status.st_mode)) {
      sessions.push_back(uuid);
    }
  }
  ::closedir(handle);
  std::sort(sessions.begin(), sessions.end());
  return sessions;
}

Result<RecoveredSession> SessionJournal::recover_directory(const std::string& directory) const {
  const auto manifest = read_manifest_from(directory);
  if (!manifest) return Result<RecoveredSession>::failure(ErrorCode::kStorage, "no recoverable session manifest");
  RecoveredSession recovered;
  recovered.manifest = *manifest.value;
  const auto names = chunk_names(directory + "/chunks");
  std::uint32_t expected_sequence = 0U;
  bool first_chunk = true;
  const std::uint32_t committed_samples = recovered.manifest.sample_count;
  for (const auto& name : names) {
    if (recovered.samples.size() >= committed_samples) {
      recovered.ignored_torn_tail = true;
      break;
    }
    const auto bytes_result = read_bytes(directory + "/chunks/" + name, kMaxJournalChunkBytes);
    if (!bytes_result || bytes_result.value->size() < kChunkHeaderBytes) {
      recovered.ignored_torn_tail = true;
      break;
    }
    const auto& bytes = *bytes_result.value;
    if (!std::equal(bytes.begin(), bytes.begin() + 4, kChunkMagic)) { recovered.ignored_torn_tail = true; break; }
    std::size_t offset = 4U;
    if (get_u16(bytes, &offset) != kChunkVersion) { recovered.ignored_torn_tail = true; break; }
    const std::uint32_t first_sequence = get_u32(bytes, &offset);
    const std::uint16_t count = get_u16(bytes, &offset);
    const std::uint32_t payload_length = get_u32(bytes, &offset);
    const std::uint32_t expected_crc = get_u32(bytes, &offset);
    if (first_chunk) expected_sequence = first_sequence;
    const std::size_t remaining_samples = static_cast<std::size_t>(committed_samples) - recovered.samples.size();
    if (first_sequence != expected_sequence || count == 0U || count > remaining_samples ||
        payload_length != static_cast<std::uint32_t>(count) * kTelemetrySampleBytes ||
        offset + payload_length != bytes.size() || crc32(bytes.data() + offset, payload_length) != expected_crc) {
      recovered.ignored_torn_tail = true;
      break;
    }
    for (std::uint16_t i = 0U; i < count; ++i) {
      const auto sample = deserialize_sample(bytes.data() + offset, kTelemetrySampleBytes);
      if (!sample || sample.value->sequence != first_sequence + static_cast<std::uint32_t>(i)) {
        recovered.ignored_torn_tail = true;
        break;
      }
      recovered.samples.push_back(*sample.value);
      offset += kTelemetrySampleBytes;
    }
    if (recovered.ignored_torn_tail) break;
    if (expected_sequence > UINT32_MAX - count) {
      recovered.ignored_torn_tail = true;
      break;
    }
    expected_sequence += count;
    first_chunk = false;
  }
  if (recovered.samples.empty()) {
    recovered.manifest.sample_count = 0U;
    recovered.manifest.last_committed_sequence = 0U;
  } else {
    recovered.manifest.sample_count = static_cast<std::uint32_t>(recovered.samples.size());
    recovered.manifest.last_committed_sequence = recovered.samples.back().sequence;
  }
  if (committed_samples != 0U && recovered.samples.size() == committed_samples &&
      recovered.samples.back().sequence != manifest.value->last_committed_sequence) {
    recovered.samples.clear();
    recovered.manifest.sample_count = 0U;
    recovered.manifest.last_committed_sequence = 0U;
    recovered.ignored_torn_tail = true;
  }
  recovered.has_valid_prefix = !recovered.samples.empty();
  // Events are deliberately parsed only enough to preserve bounded records;
  // an invalid/torn tail is ignored exactly like a sample chunk.
  const auto event_bytes = read_bytes(directory + "/events.bin", kMaxJournalEventBytes);
  if (event_bytes) {
    std::size_t offset = 0U;
    while (offset + 4U + 1U + 12U + 1U + 4U <= event_bytes.value->size()) {
      const auto& bytes = *event_bytes.value;
      if (!std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4U), "EVT1")) {
        recovered.ignored_torn_tail = true;
        break;
      }
      const std::size_t start = offset;
      const auto type = static_cast<SessionEvent::Type>(bytes[offset + 4U]);
      if (static_cast<unsigned>(type) > static_cast<unsigned>(SessionEvent::Type::kRecovery)) {
        recovered.ignored_torn_tail = true;
        break;
      }
      offset += 5U;
      const std::uint32_t wall = get_u32(bytes, &offset);
      const std::uint32_t active = get_u32(bytes, &offset);
      const std::uint32_t plan = get_u32(bytes, &offset);
      const std::size_t reason_length = bytes[offset++];
      if (reason_length > kMaxEventReasonBytes || offset + reason_length + 4U > bytes.size()) {
        recovered.ignored_torn_tail = true;
        break;
      }
      std::string reason(reinterpret_cast<const char*>(bytes.data() + offset), reason_length);
      offset += reason_length;
      const std::uint32_t stored_crc = get_u32(bytes, &offset);
      if (stored_crc != crc32(bytes.data() + start, offset - start - 4U)) {
        recovered.ignored_torn_tail = true;
        break;
      }
      recovered.events.push_back(SessionEvent{type, wall, active, plan, std::move(reason)});
    }
    if (offset != event_bytes.value->size()) recovered.ignored_torn_tail = true;
  } else {
    struct stat event_status {};
    if (::stat((directory + "/events.bin").c_str(), &event_status) == 0) {
      recovered.ignored_torn_tail = true;
    }
  }
  // Append commits counts, while final duration is written only on finish.
  // Recover clocks from the durable prefix so power loss does not create a
  // zero-timer FIT or reject valid pause events as outside the session.
  for (const auto& sample : recovered.samples) {
    recovered.manifest.wall_ms = std::max(recovered.manifest.wall_ms, sample.wall_ms);
    recovered.manifest.active_ms = std::max(recovered.manifest.active_ms, sample.active_ms);
    recovered.manifest.plan_ms = std::max(recovered.manifest.plan_ms, sample.plan_ms);
  }
  for (const auto& event : recovered.events) {
    recovered.manifest.wall_ms = std::max(recovered.manifest.wall_ms, event.wall_ms);
    recovered.manifest.active_ms = std::max(recovered.manifest.active_ms, event.active_ms);
    recovered.manifest.plan_ms = std::max(recovered.manifest.plan_ms, event.plan_ms);
  }
  return Result<RecoveredSession>::success(std::move(recovered));
}

StorageCapacity check_storage_capacity(const std::string& path, std::uint64_t required_bytes) {
  StorageCapacity result;
  result.required_bytes = required_bytes;
#ifdef ESP_PLATFORM
  // ESP-IDF's VFS does not expose POSIX statvfs. The current platform mount
  // uses the stable "/storage" mount point and the "storage" LittleFS label.
  if (path == "/storage" || path == "/storage/") {
    std::size_t total = 0U;
    std::size_t used = 0U;
    if (esp_littlefs_info("storage", &total, &used) == ESP_OK && used <= total) {
      result.available_bytes = static_cast<std::uint64_t>(total - used);
    }
  }
#else
  struct statvfs stats {};
  if (::statvfs(path.c_str(), &stats) == 0) {
    result.available_bytes = static_cast<std::uint64_t>(stats.f_bavail) * static_cast<std::uint64_t>(stats.f_frsize);
  }
#endif
  result.enough = result.available_bytes >= required_bytes;
  return result;
}

}  // namespace trainer_loop
