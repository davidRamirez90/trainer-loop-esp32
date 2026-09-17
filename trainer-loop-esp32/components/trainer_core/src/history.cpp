#include "trainer_loop/history.h"

#include "trainer_loop/limits.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>

namespace trainer_loop {
namespace {

constexpr std::size_t kMaxHistoryEntries = 64U;
constexpr std::size_t kMaxHistoryLineBytes = 4096U;

std::string hex_encode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2U);
  for (const char raw_byte : value) {
    const unsigned char byte = static_cast<unsigned char>(raw_byte);
    result.push_back(kHex[(byte >> 4U) & 0x0fU]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::optional<std::string> hex_decode(const std::string& value, std::size_t maximum) {
  if ((value.size() & 1U) != 0U || value.size() / 2U > maximum) return std::nullopt;
  std::string result;
  result.reserve(value.size() / 2U);
  for (std::size_t i = 0U; i < value.size(); i += 2U) {
    const int high = hex_value(value[i]);
    const int low = hex_value(value[i + 1U]);
    if (high < 0 || low < 0) return std::nullopt;
    result.push_back(static_cast<char>((high << 4) | low));
  }
  return result;
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

template <typename T>
std::optional<T> number(const std::string& value) {
  if (value.empty()) return std::nullopt;
  T parsed{};
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return std::nullopt;
  return parsed;
}

}  // namespace

HistoryStore::HistoryStore(std::string catalog_path) : catalog_path_(std::move(catalog_path)) {}

Status HistoryStore::load() {
  entries_.clear();
  std::ifstream input(catalog_path_);
  if (!input) return Status::ok();
  std::string line;
  while (std::getline(input, line)) {
    if (line.size() > kMaxHistoryLineBytes) {
      return Status::failure(ErrorCode::kBounds, "history catalog record is too large");
    }
    const auto fields = split(line);
    if ((fields.size() != 20U && fields.size() != 21U) || entries_.size() >= kMaxHistoryEntries || fields[0] != "S") {
      return Status::failure(ErrorCode::kStorage, "history catalog record is invalid");
    }
    const auto uuid = hex_decode(fields[1], 96U);
    const auto workout_id = hex_decode(fields[2], 96U);
    const auto workout_name = hex_decode(fields[3], kMaxWorkoutNameBytes);
    const auto fit_path = hex_decode(fields[17], 192U);
    const auto fit_hash = hex_decode(fields[18], 64U);
    const auto complete = number<std::uint32_t>(fields[4]);
    const auto interrupted = number<std::uint32_t>(fields[5]);
    const auto wall = number<std::uint32_t>(fields[6]);
    const auto active = number<std::uint32_t>(fields[7]);
    const auto plan = number<std::uint32_t>(fields[8]);
    const auto sample_count = number<std::uint32_t>(fields[9]);
    const auto valid_power_count = number<std::uint32_t>(fields[10]);
    const auto avg_power = number<std::int32_t>(fields[11]);
    const auto max_power = number<std::int32_t>(fields[12]);
    const auto avg_cadence = number<std::int32_t>(fields[13]);
    const auto avg_hr = number<std::int32_t>(fields[14]);
    const auto max_hr = number<std::int32_t>(fields[15]);
    const auto distance = number<std::uint32_t>(fields[16]);
    const auto ascent = number<std::int32_t>(fields[19]);
    if (!uuid || !workout_id || !workout_name || !fit_path || !fit_hash || !complete || !interrupted || !wall ||
        !active || !plan || !sample_count || !valid_power_count || !avg_power || !max_power || !avg_cadence ||
        !avg_hr || !max_hr || !distance || !ascent || *complete > 1U || *interrupted > 1U) {
      return Status::failure(ErrorCode::kStorage, "history catalog values are invalid");
    }
    SessionSummary summary;
    summary.uuid = *uuid;
    summary.workout_id = *workout_id;
    summary.workout_name = *workout_name;
    summary.complete = *complete != 0U;
    summary.interrupted = *interrupted != 0U;
    summary.wall_ms = *wall;
    summary.active_ms = *active;
    summary.plan_ms = *plan;
    summary.sample_count = *sample_count;
    summary.valid_power_count = *valid_power_count;
    summary.average_power_w = *avg_power;
    summary.max_power_w = *max_power;
    summary.average_cadence_rpm = *avg_cadence;
    summary.average_heart_rate_bpm = *avg_hr;
    summary.max_heart_rate_bpm = *max_hr;
    summary.distance_cm = *distance;
    summary.fit_path = *fit_path;
    summary.fit_sha256 = *fit_hash;
    summary.ascent_cm = *ascent;
    if (fields.size() == 21U) {
      const auto valid_heart_rate = number<std::uint32_t>(fields[20]);
      if (!valid_heart_rate) return Status::failure(ErrorCode::kStorage, "history heart-rate count is invalid");
      summary.valid_heart_rate_count = *valid_heart_rate;
    }
    if (summary.sample_count > kMaxSessionSamples || summary.active_ms > summary.wall_ms ||
        summary.valid_power_count > summary.sample_count || summary.valid_heart_rate_count > summary.sample_count ||
        (summary.complete && summary.interrupted)) {
      return Status::failure(ErrorCode::kStorage, "history sample coverage is invalid");
    }
    entries_.push_back(std::move(summary));
  }
  return Status::ok();
}

Status HistoryStore::persist() const {
  if (entries_.size() > kMaxHistoryEntries) {
    return Status::failure(ErrorCode::kBounds, "history catalog is too large");
  }
  std::ostringstream record;
  for (const auto& entry : entries_) {
    if (entry.uuid.empty() || entry.uuid.size() > 96U || entry.workout_id.empty() || entry.workout_id.size() > 96U ||
        entry.workout_name.empty() || entry.workout_name.size() > kMaxWorkoutNameBytes || entry.fit_path.size() > 192U ||
        entry.fit_sha256.size() > 64U) {
      return Status::failure(ErrorCode::kInvalidArgument, "history summary identity is incomplete");
    }
    record << "S|" << hex_encode(entry.uuid) << '|' << hex_encode(entry.workout_id) << '|'
           << hex_encode(entry.workout_name) << '|' << entry.complete << '|' << entry.interrupted << '|'
           << entry.wall_ms << '|' << entry.active_ms << '|' << entry.plan_ms << '|' << entry.sample_count << '|'
           << entry.valid_power_count << '|' << entry.average_power_w << '|' << entry.max_power_w << '|'
           << entry.average_cadence_rpm << '|' << entry.average_heart_rate_bpm << '|' << entry.max_heart_rate_bpm << '|'
           << entry.distance_cm << '|' << hex_encode(entry.fit_path) << '|' << hex_encode(entry.fit_sha256) << '|'
           << entry.ascent_cm << '|' << entry.valid_heart_rate_count << '\n';
  }
  const std::string temporary = catalog_path_ + ".tmp";
  std::FILE* output = std::fopen(temporary.c_str(), "wb");
  if (output == nullptr) return Status::failure(ErrorCode::kStorage, "cannot open history catalog");
  const std::string text = record.str();
  const bool written = text.empty() || std::fwrite(text.data(), 1U, text.size(), output) == text.size();
  const bool flushed = std::fflush(output) == 0;
  const int descriptor = ::fileno(output);
  const bool synced = descriptor < 0 || ::fsync(descriptor) == 0;
  const bool closed = std::fclose(output) == 0;
  if (!written || !flushed || !synced || !closed || std::rename(temporary.c_str(), catalog_path_.c_str()) != 0) {
    (void)std::remove(temporary.c_str());
    return Status::failure(ErrorCode::kStorage, "cannot publish history catalog");
  }
  return Status::ok();
}

Status HistoryStore::save(const SessionSummary& summary) {
  if (summary.uuid.empty() || summary.uuid.size() > 96U || summary.workout_id.empty() || summary.workout_id.size() > 96U ||
      summary.workout_name.empty() || summary.workout_name.size() > kMaxWorkoutNameBytes ||
      summary.fit_path.size() > 192U || summary.fit_sha256.size() > 64U ||
      summary.sample_count > kMaxSessionSamples || summary.active_ms > summary.wall_ms ||
      (summary.complete && summary.interrupted) || summary.valid_power_count > summary.sample_count ||
      summary.valid_heart_rate_count > summary.sample_count) {
    return Status::failure(ErrorCode::kInvalidArgument, "history summary identity is incomplete");
  }
  auto found = std::find_if(entries_.begin(), entries_.end(), [&](const SessionSummary& item) { return item.uuid == summary.uuid; });
  if (found != entries_.end()) {
    const SessionSummary previous = *found;
    *found = summary;
    const Status persisted = persist();
    if (!persisted) *found = previous;
    return persisted;
  }
  const bool evicted = entries_.size() >= kMaxHistoryEntries;
  SessionSummary evicted_summary;
  if (evicted) {
    evicted_summary = entries_.back();
    entries_.pop_back();
  }
  {
    entries_.insert(entries_.begin(), summary);
  }
  const Status persisted = persist();
  if (!persisted) {
    entries_.erase(entries_.begin());
    if (evicted) entries_.push_back(std::move(evicted_summary));
  }
  return persisted;
}

std::vector<SessionSummary> HistoryStore::recent(std::size_t limit) const {
  const std::size_t count = std::min(limit, entries_.size());
  return std::vector<SessionSummary>(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(count));
}

Result<SessionSummary> HistoryStore::find(const std::string& uuid) const {
  const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const SessionSummary& item) { return item.uuid == uuid; });
  if (found == entries_.end()) return Result<SessionSummary>::failure(ErrorCode::kInvalidArgument, "session is not in history");
  return Result<SessionSummary>::success(*found);
}

}  // namespace trainer_loop
