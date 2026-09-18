#include "trainer_loop/button_gesture.h"

namespace trainer_loop {

ButtonGesture::ButtonGesture(std::uint32_t debounce_ms, std::uint32_t long_press_ms)
    : debounce_ms_(debounce_ms), long_press_ms_(long_press_ms) {}

ButtonGestureEvent ButtonGesture::update(bool pressed, std::uint64_t now_ms) {
  if (!initialized_) {
    initialized_ = true;
    raw_pressed_ = pressed;
    stable_pressed_ = false;
    raw_changed_ms_ = now_ms;
  } else if (pressed != raw_pressed_) {
    raw_pressed_ = pressed;
    raw_changed_ms_ = now_ms;
  }

  if (raw_pressed_ != stable_pressed_ &&
      now_ms - raw_changed_ms_ >= debounce_ms_) {
    stable_pressed_ = raw_pressed_;
    if (stable_pressed_) {
      pressed_since_ms_ = now_ms;
      long_reported_ = false;
    } else if (!long_reported_ && now_ms - pressed_since_ms_ < long_press_ms_) {
      return ButtonGestureEvent::kShortPress;
    }
  }

  if (stable_pressed_ && !long_reported_ &&
      now_ms - pressed_since_ms_ >= long_press_ms_) {
    long_reported_ = true;
    return ButtonGestureEvent::kLongPress;
  }
  return ButtonGestureEvent::kNone;
}

}  // namespace trainer_loop
