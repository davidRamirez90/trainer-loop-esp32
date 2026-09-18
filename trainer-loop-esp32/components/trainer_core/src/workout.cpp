#include "trainer_loop/workout.h"

#include "trainer_loop/hash.h"
#include "trainer_loop/limits.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <vector>

namespace trainer_loop {
namespace {

struct Tag {
  std::string name;
  std::map<std::string, std::string> attributes;
  bool closing = false;
  bool self_closing = false;
};

std::string lower_ascii(std::string value) {
  for (char& character : value) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
  }
  return value;
}

std::string trim(std::string value) {
  std::size_t first = 0U;
  while (first < value.size() && (value[first] == ' ' || value[first] == '\t' ||
                                  value[first] == '\r' || value[first] == '\n')) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first && (value[last - 1U] == ' ' || value[last - 1U] == '\t' ||
                          value[last - 1U] == '\r' || value[last - 1U] == '\n')) {
    --last;
  }
  return value.substr(first, last - first);
}

bool is_name_character(char character) {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '_' || character == '-' ||
         character == ':' || character == '.';
}

void skip_space(std::string_view text, std::size_t* position) {
  while (*position < text.size()) {
    const char character = text[*position];
    if (character != ' ' && character != '\t' && character != '\r' && character != '\n') break;
    ++(*position);
  }
}

Result<Tag> parse_tag(std::string_view raw, std::size_t offset) {
  Tag tag;
  std::size_t position = 0U;
  skip_space(raw, &position);
  if (position < raw.size() && raw[position] == '/') {
    tag.closing = true;
    ++position;
    skip_space(raw, &position);
  }
  const std::size_t name_start = position;
  while (position < raw.size() && is_name_character(raw[position])) ++position;
  if (position == name_start) {
    return Result<Tag>::failure(ErrorCode::kMalformedInput, "XML tag has no name", offset);
  }
  tag.name = lower_ascii(std::string(raw.substr(name_start, position - name_start)));
  if (tag.closing) {
    skip_space(raw, &position);
    if (position != raw.size()) {
      return Result<Tag>::failure(ErrorCode::kMalformedInput, "closing XML tag has attributes", offset);
    }
    return Result<Tag>::success(tag);
  }

  while (position < raw.size()) {
    skip_space(raw, &position);
    if (position == raw.size()) break;
    if (raw[position] == '/') {
      tag.self_closing = true;
      ++position;
      skip_space(raw, &position);
      if (position != raw.size()) {
        return Result<Tag>::failure(ErrorCode::kMalformedInput, "text follows XML self-close", offset);
      }
      break;
    }
    const std::size_t key_start = position;
    while (position < raw.size() && is_name_character(raw[position])) ++position;
    if (position == key_start) {
      return Result<Tag>::failure(ErrorCode::kMalformedInput, "malformed XML attribute", offset + position);
    }
    const std::string key = lower_ascii(std::string(raw.substr(key_start, position - key_start)));
    skip_space(raw, &position);
    if (position == raw.size() || raw[position] != '=') {
      return Result<Tag>::failure(ErrorCode::kMalformedInput, "XML attribute has no equals sign", offset + position);
    }
    ++position;
    skip_space(raw, &position);
    if (position == raw.size() || (raw[position] != '\'' && raw[position] != '"')) {
      return Result<Tag>::failure(ErrorCode::kMalformedInput, "XML attribute must be quoted", offset + position);
    }
    const char quote = raw[position++];
    const std::size_t value_start = position;
    while (position < raw.size() && raw[position] != quote) {
      if (raw[position] == '<' || raw[position] == '&') {
        return Result<Tag>::failure(ErrorCode::kMalformedInput, "XML entities and nested tags are rejected", offset + position);
      }
      ++position;
    }
    if (position == raw.size()) {
      return Result<Tag>::failure(ErrorCode::kMalformedInput, "unterminated XML attribute", offset + value_start);
    }
    if (tag.attributes.find(key) != tag.attributes.end()) {
      return Result<Tag>::failure(ErrorCode::kMalformedInput, "duplicate XML attribute", offset + key_start);
    }
    tag.attributes[key] = std::string(raw.substr(value_start, position - value_start));
    ++position;
  }
  return Result<Tag>::success(tag);
}

std::optional<std::string> attribute(const Tag& tag, const char* name) {
  const auto found = tag.attributes.find(name);
  if (found == tag.attributes.end()) return std::nullopt;
  return found->second;
}

Result<double> number(const Tag& tag, const char* name, bool required, std::size_t offset) {
  const auto raw = attribute(tag, name);
  if (!raw.has_value()) {
    if (!required) return Result<double>::success(0.0);
    return Result<double>::failure(ErrorCode::kMalformedInput,
                                   std::string("missing attribute: ") + name, offset);
  }
  if (raw->empty() || raw->size() > 32U) {
    return Result<double>::failure(ErrorCode::kBounds, std::string("invalid attribute: ") + name, offset);
  }
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(raw->c_str(), &end);
  if (errno == ERANGE || end == raw->c_str() || *end != '\0' || !std::isfinite(value)) {
    return Result<double>::failure(ErrorCode::kMalformedInput,
                                   std::string("non-finite attribute: ") + name, offset);
  }
  return Result<double>::success(value);
}

Result<std::uint32_t> duration_ms(const Tag& tag, std::size_t offset) {
  const auto seconds = number(tag, "duration", true, offset);
  if (!seconds) return Result<std::uint32_t>::failure(seconds.error->code, seconds.error->message, offset);
  const double millis = *seconds.value * 1000.0;
  if (!std::isfinite(millis) || millis > static_cast<double>(UINT32_MAX)) {
    return Result<std::uint32_t>::failure(ErrorCode::kOverflow, "duration overflows milliseconds", offset);
  }
  const double rounded = std::round(millis);
  if (*seconds.value <= 0.0 || rounded < 1.0 || rounded > static_cast<double>(kMaxWorkoutDurationMs)) {
    return Result<std::uint32_t>::failure(ErrorCode::kBounds, "duration is outside supported bounds", offset);
  }
  return Result<std::uint32_t>::success(static_cast<std::uint32_t>(rounded));
}

Result<std::uint32_t> interval_duration_ms(double seconds, const char* field, std::size_t offset) {
  if (!std::isfinite(seconds) || seconds <= 0.0) {
    return Result<std::uint32_t>::failure(ErrorCode::kBounds,
                                           std::string(field) + " is outside supported bounds", offset);
  }
  const double millis = seconds * 1000.0;
  if (!std::isfinite(millis) || millis > static_cast<double>(UINT32_MAX)) {
    return Result<std::uint32_t>::failure(ErrorCode::kOverflow,
                                           std::string(field) + " overflows milliseconds", offset);
  }
  const double rounded = std::round(millis);
  if (rounded < 1.0 || rounded > static_cast<double>(kMaxWorkoutDurationMs)) {
    return Result<std::uint32_t>::failure(ErrorCode::kBounds,
                                           std::string(field) + " is outside supported bounds", offset);
  }
  return Result<std::uint32_t>::success(static_cast<std::uint32_t>(rounded));
}

Result<int> repeat_count(const Tag& tag, std::size_t offset) {
  const auto raw = attribute(tag, "repeat");
  if (!raw.has_value() || raw->empty() || raw->size() > 10U) {
    return Result<int>::failure(ErrorCode::kMalformedInput, "IntervalsT requires Repeat", offset);
  }
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(raw->c_str(), &end, 10);
  if (errno == ERANGE || end == raw->c_str() || *end != '\0' || value <= 0L || value > 2000L) {
    return Result<int>::failure(ErrorCode::kBounds, "IntervalsT Repeat is outside bounds", offset);
  }
  return Result<int>::success(static_cast<int>(value));
}

Result<CadenceRange> cadence(const Tag& tag, std::size_t offset) {
  const bool has_low = attribute(tag, "cadencelow").has_value();
  const bool has_high = attribute(tag, "cadencehigh").has_value();
  if (!has_low && !has_high) return Result<CadenceRange>::success(CadenceRange{});
  if (!has_low || !has_high) {
    return Result<CadenceRange>::failure(ErrorCode::kMalformedInput,
                                         "cadence requires both low and high", offset);
  }
  const auto low = number(tag, "cadencelow", true, offset);
  const auto high = number(tag, "cadencehigh", true, offset);
  if (!low || !high) return Result<CadenceRange>::failure(ErrorCode::kMalformedInput, "invalid cadence range", offset);
  if (*low.value < 0.0 || *high.value < *low.value || *high.value > 300.0) {
    return Result<CadenceRange>::failure(ErrorCode::kBounds, "cadence range is outside bounds", offset);
  }
  return Result<CadenceRange>::success(CadenceRange{*low.value, *high.value});
}

bool has_control_attribute(const Tag& tag) {
  static constexpr const char* kControlAttributes[] = {
      "duration", "power", "powerlow", "powerhigh", "onpower", "offpower",
      "onduration", "offduration", "repeat", "cadencelow", "cadencehigh"};
  for (const char* name : kControlAttributes) {
    if (tag.attributes.find(name) != tag.attributes.end()) return true;
  }
  return false;
}

bool is_known_container(const std::string& name) {
  return name == "workout_file" || name == "workout" || name == "metadata" || name == "tags";
}

bool is_metadata_tag(const std::string& name) {
  return name == "author" || name == "name" || name == "description" || name == "sporttype" ||
         name == "tags" || name == "time" || name == "tss" || name == "if" || name == "np" ||
         name == "textevent";
}

void add_warning(Workout* workout, const std::string& warning) {
  workout->has_warnings = true;
  if (workout->warnings.size() < 32U) workout->warnings.push_back(warning);
}

Status add_segment(Workout* workout, WorkoutSegment segment, const WorkoutParseLimits& limits,
                   std::size_t offset) {
  if (segment.id.empty()) segment.id = "segment-" + std::to_string(workout->segments.size() + 1U);
  if (segment.name.size() > kMaxWorkoutNameBytes || segment.phase.size() > kMaxWorkoutNameBytes) {
    return Status::failure(ErrorCode::kBounds, "segment label is too long", offset);
  }
  if (workout->segments.size() >= limits.max_segments) {
    return Status::failure(ErrorCode::kBounds, "workout has too many segments", offset);
  }
  if (segment.duration_ms == 0U) {
    return Status::failure(ErrorCode::kBounds, "segment duration is zero", offset);
  }
  const std::uint64_t total = static_cast<std::uint64_t>(workout->duration_ms()) + segment.duration_ms;
  if (total > limits.max_duration_ms) {
    return Status::failure(ErrorCode::kBounds, "workout duration exceeds four-hour limit", offset);
  }
  workout->segments.push_back(std::move(segment));
  return Status::ok();
}

Result<WorkoutSegment> parse_basic_segment(const Tag& tag, std::size_t offset) {
  const auto duration = duration_ms(tag, offset);
  if (!duration) return Result<WorkoutSegment>::failure(duration.error->code, duration.error->message, offset);
  const auto low = number(tag, "powerlow", true, offset);
  const auto high = number(tag, "powerhigh", true, offset);
  if (!low || !high) return Result<WorkoutSegment>::failure(ErrorCode::kMalformedInput, "segment power is invalid", offset);
  // PowerLow/PowerHigh are endpoint names, not an ordering guarantee. In
  // particular, a Cooldown commonly descends from a larger PowerLow to a
  // smaller PowerHigh.
  if (*low.value < 0.0 || *low.value > 10.0 || *high.value < 0.0 || *high.value > 10.0) {
    return Result<WorkoutSegment>::failure(ErrorCode::kBounds, "segment power is outside bounds", offset);
  }
  const auto cadence_value = cadence(tag, offset);
  if (!cadence_value) return Result<WorkoutSegment>::failure(cadence_value.error->code, cadence_value.error->message, offset);
  WorkoutSegment segment;
  segment.duration_ms = *duration.value;
  // ZWO Warmup, Cooldown, and Ramp all describe a linear power transition
  // from PowerLow to PowerHigh. A one-point transition remains harmless, but
  // treating the bounds as a step would silently change ordinary warmups.
  segment.kind = SegmentKind::kRamp;
  segment.power_origin = PowerOrigin::kFractionFtp;
  segment.start_low = *low.value;
  segment.start_high = *low.value;
  segment.end_low = *high.value;
  segment.end_high = *high.value;
  segment.phase = tag.name == "warmup" ? "Warmup" : (tag.name == "cooldown" ? "Cooldown" : "Workout");
  segment.name = segment.phase;
  if (cadence_value.value->high_rpm > 0.0) segment.cadence = *cadence_value.value;
  return Result<WorkoutSegment>::success(std::move(segment));
}

Result<WorkoutSegment> parse_steady_state(const Tag& tag, std::size_t offset) {
  const auto duration = duration_ms(tag, offset);
  const bool has_power = attribute(tag, "power").has_value();
  const bool has_low = attribute(tag, "powerlow").has_value();
  const bool has_high = attribute(tag, "powerhigh").has_value();
  if (!duration || (has_power == (has_low || has_high))) {
    return Result<WorkoutSegment>::failure(ErrorCode::kMalformedInput, "SteadyState is invalid", offset);
  }
  double low = 0.0;
  double high = 0.0;
  if (has_power) {
    const auto power = number(tag, "power", true, offset);
    if (!power) return Result<WorkoutSegment>::failure(ErrorCode::kMalformedInput, "SteadyState is invalid", offset);
    low = high = *power.value;
  } else {
    const auto low_value = number(tag, "powerlow", true, offset);
    const auto high_value = number(tag, "powerhigh", true, offset);
    if (!low_value || !high_value) {
      return Result<WorkoutSegment>::failure(ErrorCode::kMalformedInput, "SteadyState is invalid", offset);
    }
    low = *low_value.value;
    high = *high_value.value;
  }
  if (low < 0.0 || high < low || high > 10.0) {
    return Result<WorkoutSegment>::failure(ErrorCode::kBounds, "SteadyState power is outside bounds", offset);
  }
  const auto cadence_value = cadence(tag, offset);
  if (!cadence_value) return Result<WorkoutSegment>::failure(cadence_value.error->code, cadence_value.error->message, offset);
  WorkoutSegment segment;
  segment.duration_ms = *duration.value;
  segment.kind = SegmentKind::kStep;
  segment.power_origin = PowerOrigin::kFractionFtp;
  segment.start_low = low;
  segment.start_high = high;
  segment.end_low = low;
  segment.end_high = high;
  segment.phase = "Workout";
  segment.name = "Steady state";
  if (cadence_value.value->high_rpm > 0.0) segment.cadence = *cadence_value.value;
  return Result<WorkoutSegment>::success(std::move(segment));
}

std::string metadata_text(const std::string& source, const std::string& tag_name) {
  const std::string open = "<" + tag_name + ">";
  const std::string close = "</" + tag_name + ">";
  const std::size_t begin = source.find(open);
  if (begin == std::string::npos) return {};
  const std::size_t text_begin = begin + open.size();
  const std::size_t end = source.find(close, text_begin);
  if (end == std::string::npos || end - text_begin > kMaxWorkoutTextBytes) return {};
  return trim(source.substr(text_begin, end - text_begin));
}

}  // namespace

Result<Workout> parse_zwo(const std::string& source, const WorkoutParseLimits& limits) {
  if (source.empty() || source.size() > limits.max_source_bytes || source.size() > kMaxWorkoutSourceBytes) {
    return Result<Workout>::failure(ErrorCode::kBounds, "ZWO source exceeds the bounded cache limit");
  }
  if (source.find("<!DOCTYPE") != std::string::npos || source.find("<!doctype") != std::string::npos ||
      source.find("<!ENTITY") != std::string::npos || source.find("<!entity") != std::string::npos ||
      source.find('&') != std::string::npos) {
    return Result<Workout>::failure(ErrorCode::kMalformedInput, "XML entities and DTDs are not supported");
  }

  Workout workout;
  workout.source_size = static_cast<std::uint32_t>(source.size());
  workout.source_hash = hex_u32(fnv1a32(source));
  workout.id = workout.source_hash;
  workout.name = metadata_text(source, "name");
  workout.description = metadata_text(source, "description");
  workout.sport = metadata_text(source, "sporttype");
  if (workout.name.empty()) workout.name = "Workout";
  if (workout.name.size() > kMaxWorkoutNameBytes || workout.description.size() > kMaxWorkoutTextBytes) {
    return Result<Workout>::failure(ErrorCode::kBounds, "workout metadata is too long");
  }

  const std::string_view view(source);
  std::vector<std::string> stack;
  bool root_seen = false;
  bool root_closed = false;
  std::size_t position = 0U;
  while (position < view.size()) {
    const std::size_t open = view.find('<', position);
    if (open == std::string_view::npos) {
      const std::string text = trim(std::string(view.substr(position)));
      if (!text.empty() && (root_closed || !root_seen)) {
        return Result<Workout>::failure(ErrorCode::kMalformedInput, "text is outside workout_file root", position);
      }
      if (!text.empty() && text.find('&') != std::string::npos) {
        return Result<Workout>::failure(ErrorCode::kMalformedInput, "XML entity in text", position);
      }
      break;
    }
    const std::string text_between = trim(std::string(view.substr(position, open - position)));
    if (!text_between.empty() && (root_closed || !root_seen)) {
      return Result<Workout>::failure(ErrorCode::kMalformedInput, "text is outside workout_file root", position);
    }
    if (view.substr(position, open - position).find('&') != std::string_view::npos) {
      return Result<Workout>::failure(ErrorCode::kMalformedInput, "XML entity in text", position);
    }
    if (view.substr(open, 4U) == "<!--") {
      const std::size_t close_comment = view.find("-->", open + 4U);
      if (close_comment == std::string_view::npos) {
        return Result<Workout>::failure(ErrorCode::kMalformedInput, "unterminated XML comment", open);
      }
      position = close_comment + 3U;
      continue;
    }
    if (view.substr(open, 2U) == "<?") {
      const std::size_t close_pi = view.find("?>", open + 2U);
      if (close_pi == std::string_view::npos) {
        return Result<Workout>::failure(ErrorCode::kMalformedInput, "unterminated XML declaration", open);
      }
      position = close_pi + 2U;
      continue;
    }
    const std::size_t close = [&]() {
      bool quoted = false;
      char quote = 0;
      for (std::size_t i = open + 1U; i < view.size(); ++i) {
        if ((view[i] == '\'' || view[i] == '"') && (!quoted || view[i] == quote)) {
          if (quoted) quoted = false;
          else { quoted = true; quote = view[i]; }
        } else if (view[i] == '>' && !quoted) {
          return i;
        }
      }
      return std::string_view::npos;
    }();
    if (close == std::string_view::npos) {
      return Result<Workout>::failure(ErrorCode::kMalformedInput, "unterminated XML tag", open);
    }
    const auto tag_result = parse_tag(view.substr(open + 1U, close - open - 1U), open);
    if (!tag_result) return Result<Workout>::failure(tag_result.error->code, tag_result.error->message, tag_result.error->offset);
    const Tag& tag = *tag_result.value;
    if (tag.name == "!doctype" || tag.name == "!entity" || tag.name == "![cdata") {
      return Result<Workout>::failure(ErrorCode::kMalformedInput, "DTD or CDATA is not supported", open);
    }
    if (tag.closing) {
      if (stack.empty() || stack.back() != tag.name) {
        return Result<Workout>::failure(ErrorCode::kMalformedInput, "mismatched XML closing tag", open);
      }
      stack.pop_back();
      if (stack.empty()) root_closed = true;
      position = close + 1U;
      continue;
    }

    if (!root_seen) {
      if (tag.name != "workout_file") {
        return Result<Workout>::failure(ErrorCode::kMalformedInput, "workout_file must be the XML root", open);
      }
      root_seen = true;
    } else if (root_closed || stack.empty()) {
      return Result<Workout>::failure(ErrorCode::kMalformedInput, "multiple XML roots are not supported", open);
    }
    if (tag.name == "freeride") {
      return Result<Workout>::failure(ErrorCode::kUnsupported,
                                      "FreeRide blocks are unsupported in P0", open);
    }
    if (tag.name == "intervalst") {
      const auto repeat = repeat_count(tag, open);
      const auto on_duration = number(tag, "onduration", true, open);
      const auto off_duration = number(tag, "offduration", true, open);
      const auto on_power = number(tag, "onpower", true, open);
      const auto off_power = number(tag, "offpower", true, open);
      if (!repeat || !on_duration || !off_duration || !on_power || !off_power) {
        return Result<Workout>::failure(ErrorCode::kMalformedInput, "IntervalsT is incomplete", open);
      }
      if (*on_duration.value <= 0.0 || *off_duration.value <= 0.0 || *on_power.value < 0.0 ||
          *off_power.value < 0.0 || *on_power.value > 10.0 || *off_power.value > 10.0) {
        return Result<Workout>::failure(ErrorCode::kBounds, "IntervalsT value is outside bounds", open);
      }
      const auto on_duration_ms = interval_duration_ms(*on_duration.value, "IntervalsT OnDuration", open);
      const auto off_duration_ms = interval_duration_ms(*off_duration.value, "IntervalsT OffDuration", open);
      if (!on_duration_ms || !off_duration_ms) {
        const auto& error = !on_duration_ms ? on_duration_ms.error : off_duration_ms.error;
        return Result<Workout>::failure(error->code, error->message, open);
      }
      for (int index = 0; index < *repeat.value; ++index) {
        WorkoutSegment on;
        on.duration_ms = *on_duration_ms.value;
        on.id = "interval-" + std::to_string(workout.segments.size() + 1U);
        on.name = "On";
        on.phase = "Intervals";
        on.power_origin = PowerOrigin::kFractionFtp;
        on.start_low = on.start_high = on.end_low = on.end_high = *on_power.value;
        Status status = add_segment(&workout, std::move(on), limits, open);
        if (!status) return Result<Workout>::failure(status.error->code, status.error->message, open);
        WorkoutSegment off;
        off.duration_ms = *off_duration_ms.value;
        off.id = "interval-" + std::to_string(workout.segments.size() + 1U);
        off.name = "Off";
        off.phase = "Recovery";
        off.power_origin = PowerOrigin::kFractionFtp;
        off.start_low = off.start_high = off.end_low = off.end_high = *off_power.value;
        status = add_segment(&workout, std::move(off), limits, open);
        if (!status) return Result<Workout>::failure(status.error->code, status.error->message, open);
      }
    } else if (tag.name == "warmup" || tag.name == "cooldown" || tag.name == "ramp") {
      const auto segment = parse_basic_segment(tag, open);
      if (!segment) return Result<Workout>::failure(segment.error->code, segment.error->message, open);
      Status status = add_segment(&workout, *segment.value, limits, open);
      if (!status) return Result<Workout>::failure(status.error->code, status.error->message, open);
    } else if (tag.name == "steadystate") {
      const auto segment = parse_steady_state(tag, open);
      if (!segment) return Result<Workout>::failure(segment.error->code, segment.error->message, open);
      Status status = add_segment(&workout, *segment.value, limits, open);
      if (!status) return Result<Workout>::failure(status.error->code, status.error->message, open);
    } else if (is_known_container(tag.name) || is_metadata_tag(tag.name)) {
      // Metadata/text is intentionally ignored by the control model. It is
      // still parsed as XML so malformed nesting cannot hide a control tag.
    } else if (has_control_attribute(tag)) {
      return Result<Workout>::failure(ErrorCode::kUnsupported,
                                      "unknown control-bearing XML tag: " + tag.name, open);
    } else {
      add_warning(&workout, "ignored metadata tag: " + tag.name);
    }

    if (!tag.self_closing) {
      stack.push_back(tag.name);
    } else if (stack.empty()) {
      root_closed = true;
    }
    position = close + 1U;
  }
  if (!stack.empty()) return Result<Workout>::failure(ErrorCode::kMalformedInput, "unclosed XML tag");
  if (!root_seen) return Result<Workout>::failure(ErrorCode::kMalformedInput, "missing workout_file root");
  if (workout.segments.empty()) return Result<Workout>::failure(ErrorCode::kMalformedInput, "workout has no supported segments");
  const Status valid = validate_workout(workout, kFtpMinWatts);
  if (!valid) return Result<Workout>::failure(valid.error->code, valid.error->message);
  return Result<Workout>::success(std::move(workout));
}

Status validate_workout(const Workout& workout, int ftp_watts) {
  if (workout.segments.empty()) return Status::failure(ErrorCode::kMalformedInput, "workout has no segments");
  if (workout.segments.size() > kMaxWorkoutSegments) return Status::failure(ErrorCode::kBounds, "too many workout segments");
  if (workout.name.empty() || workout.name.size() > kMaxWorkoutNameBytes) {
    return Status::failure(ErrorCode::kBounds, "workout name is invalid");
  }
  if (ftp_watts < kFtpMinWatts || ftp_watts > kFtpMaxWatts) {
    return Status::failure(ErrorCode::kBounds, "FTP is outside supported bounds");
  }
  std::uint64_t total = 0U;
  for (const auto& segment : workout.segments) {
    if (segment.duration_ms == 0U || segment.id.empty() || segment.id.size() > kMaxWorkoutNameBytes ||
        segment.name.size() > kMaxWorkoutNameBytes || segment.phase.size() > kMaxWorkoutNameBytes) {
      return Status::failure(ErrorCode::kMalformedInput, "segment identity or duration is invalid");
    }
    if (segment.kind != SegmentKind::kStep && segment.kind != SegmentKind::kRamp) {
      return Status::failure(ErrorCode::kUnsupported, "segment kind is unsupported");
    }
    if (segment.power_origin != PowerOrigin::kFractionFtp && segment.power_origin != PowerOrigin::kAbsoluteWatts) {
      return Status::failure(ErrorCode::kUnsupported, "segment power origin is unsupported");
    }
    if (!std::isfinite(segment.start_low) || !std::isfinite(segment.start_high) ||
        !std::isfinite(segment.end_low) || !std::isfinite(segment.end_high)) {
      return Status::failure(ErrorCode::kMalformedInput, "segment power is non-finite");
    }
    if (segment.start_low < 0.0 || segment.start_high < segment.start_low ||
        segment.end_low < 0.0 || segment.end_high < segment.end_low) {
      return Status::failure(ErrorCode::kBounds, "segment power range is invalid");
    }
    if (segment.cadence.has_value() &&
        (!std::isfinite(segment.cadence->low_rpm) || !std::isfinite(segment.cadence->high_rpm) ||
         segment.cadence->low_rpm < 0.0 || segment.cadence->high_rpm < segment.cadence->low_rpm ||
         segment.cadence->high_rpm > 300.0)) {
      return Status::failure(ErrorCode::kBounds, "segment cadence range is invalid");
    }
    const double multiplier = segment.power_origin == PowerOrigin::kFractionFtp
                                  ? static_cast<double>(ftp_watts)
                                  : 1.0;
    if (segment.end_high * multiplier > static_cast<double>(kPowerMaxWatts) ||
        segment.start_high * multiplier > static_cast<double>(kPowerMaxWatts)) {
      return Status::failure(ErrorCode::kBounds, "segment exceeds product power limit");
    }
    total += segment.duration_ms;
    if (total > kMaxWorkoutDurationMs) return Status::failure(ErrorCode::kBounds, "workout duration exceeds product limit");
  }
  return Status::ok();
}

TargetRange target_range_at(const Workout& workout, std::uint32_t plan_ms) {
  if (workout.segments.empty()) return TargetRange{};
  std::uint64_t cursor = 0U;
  const WorkoutSegment* selected = &workout.segments.back();
  std::uint32_t local_ms = selected->duration_ms;
  for (const auto& segment : workout.segments) {
    const std::uint64_t end = cursor + segment.duration_ms;
    if (static_cast<std::uint64_t>(plan_ms) < end) {
      selected = &segment;
      local_ms = plan_ms > cursor ? static_cast<std::uint32_t>(plan_ms - cursor) : 0U;
      break;
    }
    cursor = end;
  }
  if (selected->kind == SegmentKind::kStep || selected->duration_ms == 0U) {
    return TargetRange{selected->start_low, selected->start_high};
  }
  const double ratio = static_cast<double>(local_ms) / static_cast<double>(selected->duration_ms);
  return TargetRange{
      selected->start_low + (selected->end_low - selected->start_low) * ratio,
      selected->start_high + (selected->end_high - selected->start_high) * ratio};
}

TargetRange resolved_target_range_at(const Workout& workout, std::uint32_t plan_ms, int ftp_watts) {
  const TargetRange range = target_range_at(workout, plan_ms);
  const WorkoutSegment* selected = nullptr;
  std::uint64_t cursor = 0U;
  for (const auto& segment : workout.segments) {
    if (static_cast<std::uint64_t>(plan_ms) < cursor + segment.duration_ms) {
      selected = &segment;
      break;
    }
    cursor += segment.duration_ms;
  }
  if (selected == nullptr && !workout.segments.empty()) selected = &workout.segments.back();
  const double multiplier = selected != nullptr && selected->power_origin == PowerOrigin::kFractionFtp
                                ? static_cast<double>(ftp_watts)
                                : 1.0;
  return TargetRange{range.low * multiplier, range.high * multiplier};
}

Result<std::vector<int>> resolve_segment_targets(const Workout& workout, int ftp_watts) {
  const Status valid = validate_workout(workout, ftp_watts);
  if (!valid) return Result<std::vector<int>>::failure(valid.error->code, valid.error->message);
  std::vector<int> targets;
  targets.reserve(workout.segments.size());
  for (const auto& segment : workout.segments) {
    const double multiplier = segment.power_origin == PowerOrigin::kFractionFtp
                                  ? static_cast<double>(ftp_watts)
                                  : 1.0;
    targets.push_back(static_cast<int>(std::lround(((segment.start_low + segment.start_high) / 2.0) * multiplier)));
  }
  return Result<std::vector<int>>::success(std::move(targets));
}

}  // namespace trainer_loop
