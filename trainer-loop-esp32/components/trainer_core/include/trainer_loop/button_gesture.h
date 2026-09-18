#pragma once

#include <cstdint>

namespace trainer_loop {

enum class ButtonGestureEvent : std::uint8_t {
  kNone,
  kShortPress,
  kLongPress,
};

// Converts a noisy active-high button signal into one-shot press events.
// Long presses are reported but intentionally have no destructive meaning at
// this layer; callers choose a safe action for each event.
class ButtonGesture {
 public:
  explicit ButtonGesture(std::uint32_t debounce_ms = 40U,
                         std::uint32_t long_press_ms = 1000U);

  ButtonGestureEvent update(bool pressed, std::uint64_t now_ms);

 private:
  std::uint32_t debounce_ms_;
  std::uint32_t long_press_ms_;
  bool initialized_ = false;
  bool raw_pressed_ = false;
  bool stable_pressed_ = false;
  bool long_reported_ = false;
  std::uint64_t raw_changed_ms_ = 0U;
  std::uint64_t pressed_since_ms_ = 0U;
};

}  // namespace trainer_loop
