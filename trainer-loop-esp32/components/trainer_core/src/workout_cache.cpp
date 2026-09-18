#include "trainer_loop/storage_io.h"
#include "trainer_loop/workout_cache.h"

#include "trainer_loop/hash.h"
#include "trainer_loop/limits.h"
#include "trainer_loop/telemetry.h"
#include "trainer_loop/workout.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace trainer_loop {
namespace {

bool ensure_directory(const std::string& path) {
  if (::mkdir(path.c_str(), 0700) == 0) return true;
  return errno == EEXIST;
}

bool safe_component(const std::string& value) {
  if (value.empty() || value.size() > 96U) return false;
  for (const char character : value) {
    if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '-' || character == '_')) return false;
  }
  return true;
}

std::string hex_encode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2U);
  for (const char raw_byte : value) {
    const unsigned char byte = static_cast<unsigned char>(raw_byte);
    out.push_back(kHex[(byte >> 4U) & 0x0fU]);
    out.push_back(kHex[byte & 0x0fU]);
  }
  return out;
}

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::optional<std::string> hex_decode(const std::string& value, std::size_t maximum) {
  if ((value.size() & 1U) != 0U || value.size() / 2U > maximum) return std::nullopt;
  std::string out;
  out.reserve(value.size() / 2U);
  for (std::size_t i = 0U; i < value.size(); i += 2U) {
    const int high = hex_value(value[i]);
    const int low = hex_value(value[i + 1U]);
    if (high < 0 || low < 0) return std::nullopt;
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return out;
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

std::optional<double> decimal(const std::string& value) {
  char* end = nullptr;
  errno = 0;
  const double result = std::strtod(value.c_str(), &end);
  if (errno == ERANGE || end == value.c_str() || *end != '\0' || !std::isfinite(result)) return std::nullopt;
  return result;
}

std::optional<std::int64_t> integer(const std::string& value) {
  if (value.empty()) return std::nullopt;
  std::int64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
  return result;
}

std::string segment_line(const WorkoutSegment& segment) {
  std::ostringstream line;
  line << "S|" << segment.duration_ms << '|' << hex_encode(segment.id) << '|' << hex_encode(segment.name) << '|'
       << hex_encode(segment.phase) << '|' << static_cast<unsigned>(segment.kind) << '|'
       << static_cast<unsigned>(segment.power_origin) << '|' << std::setprecision(17)
       << segment.start_low << '|' << segment.start_high << '|' << segment.end_low << '|' << segment.end_high << '|'
       << (segment.cadence.has_value() ? 1 : 0) << '|'
       << (segment.cadence.has_value() ? segment.cadence->low_rpm : 0.0) << '|'
       << (segment.cadence.has_value() ? segment.cadence->high_rpm : 0.0) << '\n';
  return line.str();
}

Result<CachedWorkout> decode(const std::string& text) {
  const std::size_t crc_marker = text.rfind("CRC|");
  if (crc_marker == std::string::npos || crc_marker == 0U || text.back() != '\n') {
    return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout has no checksum");
  }
  const std::string body = text.substr(0U, crc_marker);
  const std::string checksum = text.substr(crc_marker + 4U, text.size() - crc_marker - 5U);
  const auto expected = integer(checksum);
  if (!expected || *expected < 0 || *expected > static_cast<std::int64_t>(UINT32_MAX) ||
      static_cast<std::uint32_t>(*expected) != crc32(reinterpret_cast<const std::uint8_t*>(body.data()), body.size())) {
    return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout checksum mismatch");
  }
  std::istringstream lines(body);
  std::string line;
  CachedWorkout cached;
  std::size_t segment_count = 0U;
  bool header_seen = false;
  bool workout_seen = false;
  while (std::getline(lines, line)) {
    const auto fields = split(line);
    if (fields.empty()) continue;
    if (fields[0] == "H") {
      if (fields.size() != 12U || header_seen) {
        return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout header is duplicated or corrupt");
      }
      header_seen = true;
      const auto athlete = hex_decode(fields[1], kMaxAthleteIdBytes);
      const auto event = hex_decode(fields[2], 96U);
      const auto date = hex_decode(fields[3], 16U);
      const auto timezone = hex_decode(fields[4], kMaxTimezoneBytes);
      const auto name = hex_decode(fields[5], kMaxWorkoutNameBytes);
      const auto source = hex_decode(fields[6], kMaxWorkoutSourceBytes);
      const auto ftp = integer(fields[7]);
      const auto downloaded = integer(fields[8]);
      const auto validated = integer(fields[9]);
      const auto source_size = integer(fields[10]);
      const auto schema = integer(fields[11]);
      if (!athlete || !event || !date || !timezone || !name || !source || !ftp || !downloaded || !validated || !source_size ||
          !schema || *schema != 0 ||
          *source_size < 0 || static_cast<std::uint64_t>(*source_size) > kMaxWorkoutSourceBytes ||
          *ftp < kFtpMinWatts || *ftp > kFtpMaxWatts ||
          event->empty() || event->size() > 96U || date->empty() || date->size() > 16U || timezone->empty() ||
          name->empty() || source->empty()) {
        return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout header is corrupt");
      }
      cached.athlete_id = *athlete;
      cached.event_id = *event;
      cached.local_date = *date;
      cached.timezone = *timezone;
      cached.name = *name;
      cached.source_zwo = *source;
      cached.ftp_used_for_preview = static_cast<int>(*ftp);
      cached.downloaded_utc_ms = *downloaded;
      cached.validated_utc_ms = *validated;
      cached.workout.source_size = static_cast<std::uint32_t>(*source_size);
    } else if (fields[0] == "W") {
      if (fields.size() != 6U || workout_seen) {
        return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout identity is duplicated or corrupt");
      }
      workout_seen = true;
      const auto id = hex_decode(fields[1], 96U);
      const auto name = hex_decode(fields[2], kMaxWorkoutNameBytes);
      const auto description = hex_decode(fields[3], kMaxWorkoutTextBytes);
      const auto hash = hex_decode(fields[4], 8U);
      const auto parser = hex_decode(fields[5], 32U);
      if (!id || !name || !description || !hash || !parser) return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout identity is corrupt");
      if (id->empty() || name->empty() || hash->size() != 8U || parser->empty()) {
        return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout identity is invalid");
      }
      cached.workout.id = *id;
      cached.workout.name = *name;
      cached.workout.description = *description;
      cached.workout.source_hash = *hash;
      cached.workout.parser_version = *parser;
    } else if (fields[0] == "S") {
      if (fields.size() != 14U) {
        return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout segment is corrupt");
      }
      const auto duration = integer(fields[1]);
      const auto id = hex_decode(fields[2], 96U);
      const auto name = hex_decode(fields[3], kMaxWorkoutNameBytes);
      const auto phase = hex_decode(fields[4], kMaxWorkoutTextBytes);
      const auto kind = integer(fields[5]);
      const auto origin = integer(fields[6]);
      const auto start_low = decimal(fields[7]);
      const auto start_high = decimal(fields[8]);
      const auto end_low = decimal(fields[9]);
      const auto end_high = decimal(fields[10]);
      const auto cadence_present = integer(fields[11]);
      const auto cadence_low = decimal(fields[12]);
      const auto cadence_high = decimal(fields[13]);
      if (!duration || !id || !name || !phase || !kind || !origin || !start_low || !start_high || !end_low || !end_high ||
          !cadence_present || !cadence_low || !cadence_high || *duration <= 0 || *duration > kMaxWorkoutDurationMs ||
          *kind < 0 || *kind > 1 || *origin < 0 || *origin > 1 || *cadence_present < 0 || *cadence_present > 1 ||
          id->empty() || name->empty()) {
        return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout segment is corrupt");
      }
      if (segment_count >= kMaxWorkoutSegments) {
        return Result<CachedWorkout>::failure(ErrorCode::kBounds, "cached workout has too many segments");
      }
      WorkoutSegment segment;
      segment.duration_ms = static_cast<std::uint32_t>(*duration);
      segment.id = *id;
      segment.name = *name;
      segment.phase = *phase;
      segment.kind = static_cast<SegmentKind>(*kind);
      segment.power_origin = static_cast<PowerOrigin>(*origin);
      segment.start_low = *start_low;
      segment.start_high = *start_high;
      segment.end_low = *end_low;
      segment.end_high = *end_high;
      if (*cadence_present != 0) segment.cadence = CadenceRange{*cadence_low, *cadence_high};
      cached.workout.segments.push_back(std::move(segment));
      ++segment_count;
    } else {
      return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout record has an unknown row");
    }
  }
  if (!header_seen || !workout_seen || cached.event_id.empty() || cached.workout.segments.empty() || segment_count > kMaxWorkoutSegments ||
      cached.source_zwo.size() != cached.workout.source_size ||
      cached.workout.source_hash != hex_u32(fnv1a32(cached.source_zwo))) {
    return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout is incomplete");
  }
  const Status valid = validate_workout(cached.workout, cached.ftp_used_for_preview);
  if (!valid) return Result<CachedWorkout>::failure(valid.error->code, valid.error->message);
  return Result<CachedWorkout>::success(std::move(cached));
}

}  // namespace

WorkoutCache::WorkoutCache(std::string directory) : directory_(std::move(directory)) {}

Status WorkoutCache::initialize() {
  return ensure_directory(directory_) ? Status::ok() : Status::failure(ErrorCode::kStorage, "cannot create workout cache");
}

std::string WorkoutCache::path_for(const std::string& event_id) const {
  return directory_ + "/" + event_id + ".cache";
}

Status WorkoutCache::save(const CachedWorkout& cached) {
  if (!safe_component(cached.event_id) || cached.athlete_id.empty() || cached.athlete_id.size() > kMaxAthleteIdBytes ||
      cached.local_date.empty() || cached.local_date.size() > 16U || cached.timezone.empty() ||
      cached.timezone.size() > kMaxTimezoneBytes || cached.name.empty() || cached.name.size() > kMaxWorkoutNameBytes ||
      cached.source_zwo.empty() || cached.source_zwo.size() > kMaxWorkoutSourceBytes ||
      cached.workout.segments.size() > kMaxWorkoutSegments ||
      cached.workout.source_hash != hex_u32(fnv1a32(cached.source_zwo)) || cached.workout.source_size != cached.source_zwo.size()) {
    return Status::failure(ErrorCode::kBounds, "cached workout exceeds bounds");
  }
  const Status valid = validate_workout(cached.workout, cached.ftp_used_for_preview);
  if (!valid) return valid;
  const Status initialized = initialize();
  if (!initialized) return initialized;
  std::ostringstream body;
  body << "H|" << hex_encode(cached.athlete_id) << '|' << hex_encode(cached.event_id) << '|' << hex_encode(cached.local_date)
       << '|' << hex_encode(cached.timezone) << '|' << hex_encode(cached.name) << '|' << hex_encode(cached.source_zwo) << '|'
       << cached.ftp_used_for_preview << '|' << cached.downloaded_utc_ms << '|' << cached.validated_utc_ms << '|'
       << cached.source_zwo.size() << "|0\n";
  body << "W|" << hex_encode(cached.workout.id) << '|' << hex_encode(cached.workout.name) << '|'
       << hex_encode(cached.workout.description) << '|' << hex_encode(cached.workout.source_hash) << '|'
       << hex_encode(cached.workout.parser_version) << '\n';
  for (const auto& segment : cached.workout.segments) body << segment_line(segment);
  const std::string text = body.str();
  const std::uint32_t checksum = crc32(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  std::ostringstream record;
  record << text << "CRC|" << checksum << '\n';
  const std::string temporary = path_for(cached.event_id) + ".tmp";
  const std::string serialized = record.str();
  std::FILE* output = std::fopen(temporary.c_str(), "wb");
  if (output == nullptr) return Status::failure(ErrorCode::kStorage, "cannot open workout cache record");
  const bool written = storage_write(serialized.data(), serialized.size(), output) == serialized.size();
  const bool flushed = std::fflush(output) == 0;
  const int descriptor = ::fileno(output);
  const bool synced = descriptor < 0 || ::fsync(descriptor) == 0;
  const bool closed = std::fclose(output) == 0;
  if (!written || !flushed || !synced || !closed || std::rename(temporary.c_str(), path_for(cached.event_id).c_str()) != 0) {
    (void)std::remove(temporary.c_str());
    return Status::failure(ErrorCode::kStorage, "cannot publish workout cache record");
  }
  return Status::ok();
}

Status WorkoutCache::replace_for_date(const std::string& local_date,
                                      const std::vector<CachedWorkout>& workouts) {
  if (local_date.empty() || local_date.size() > 16U) {
    return Status::failure(ErrorCode::kInvalidArgument, "cache replacement date is invalid");
  }
  const Status initialized = initialize();
  if (!initialized) return initialized;
  std::vector<std::string> keep;
  keep.reserve(workouts.size());
  for (const CachedWorkout& cached : workouts) {
    if (cached.local_date != local_date) {
      return Status::failure(ErrorCode::kInvalidArgument,
                             "cached workout date does not match replacement date");
    }
    if (std::find(keep.begin(), keep.end(), cached.event_id) != keep.end()) {
      return Status::failure(ErrorCode::kInvalidArgument, "duplicate workout event in replacement");
    }
    keep.push_back(cached.event_id);
  }

  // Publish every new record before pruning old records. A failed download or
  // write therefore never authorizes deletion of the prior day's cache.
  for (const CachedWorkout& cached : workouts) {
    const Status saved = save(cached);
    if (!saved) return saved;
  }

  DIR* handle = ::opendir(directory_.c_str());
  if (handle == nullptr) return Status::ok();
  Status result = Status::ok();
  while (const dirent* entry = ::readdir(handle)) {
    const std::string filename(entry->d_name);
    if (filename.size() <= 6U || filename.substr(filename.size() - 6U) != ".cache") continue;
    const std::string event_id = filename.substr(0U, filename.size() - 6U);
    const auto cached = load(event_id);
    if (!cached || cached.value->local_date != local_date ||
        std::find(keep.begin(), keep.end(), event_id) != keep.end()) {
      continue;
    }
    if (::remove(path_for(event_id).c_str()) != 0) {
      result = Status::failure(ErrorCode::kStorage, "cannot prune replaced workout cache");
      break;
    }
  }
  ::closedir(handle);
  return result;
}

Result<CachedWorkout> WorkoutCache::load(const std::string& event_id) const {
  if (!safe_component(event_id)) return Result<CachedWorkout>::failure(ErrorCode::kInvalidArgument, "invalid event ID");
  std::ifstream input(path_for(event_id), std::ios::binary);
  if (!input) return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout is unavailable");
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > (kMaxWorkoutSourceBytes * 2U + 65536U)) {
    return Result<CachedWorkout>::failure(ErrorCode::kBounds, "cached workout record is too large");
  }
  input.seekg(0, std::ios::beg);
  std::string text(static_cast<std::size_t>(size), '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!input && !text.empty()) return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout read failed");
  const auto cached = decode(text);
  if (cached && cached.value->event_id != event_id) {
    return Result<CachedWorkout>::failure(ErrorCode::kStorage, "cached workout event ID does not match its path");
  }
  return cached;
}

std::vector<CachedWorkout> WorkoutCache::list_for_date(const std::string& local_date) const {
  std::vector<CachedWorkout> result;
  DIR* handle = ::opendir(directory_.c_str());
  if (handle == nullptr) return result;
  while (const dirent* entry = ::readdir(handle)) {
    const std::string filename(entry->d_name);
    if (filename.size() <= 6U || filename.substr(filename.size() - 6U) != ".cache") continue;
    const std::string event_id = filename.substr(0U, filename.size() - 6U);
    const auto cached = load(event_id);
    if (cached && cached.value->local_date == local_date) result.push_back(std::move(*cached.value));
  }
  ::closedir(handle);
  return result;
}

}  // namespace trainer_loop
