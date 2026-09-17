#pragma once

#include "trainer_loop/result.h"

#include <cstdint>

namespace trainer_loop::board_v1 {

// Source-verified primary buses for the documented Waveshare V1 board. Reset
// and rail enables are deliberately not represented as ESP32 GPIOs: they are
// outputs of the TCA/IO expander and stay behind this board boundary.
struct PinMap {
  static constexpr int kDisplayWidth = 368;
  static constexpr int kDisplayHeight = 448;
  static constexpr int kDisplayCs = 12;
  static constexpr int kDisplayClock = 11;
  static constexpr int kDisplayD0 = 4;
  static constexpr int kDisplayD1 = 5;
  static constexpr int kDisplayD2 = 6;
  static constexpr int kDisplayD3 = 7;
  static constexpr int kI2cSda = 15;
  static constexpr int kI2cScl = 14;
  static constexpr int kExpanderAddress = 0x20;
  static constexpr int kTouchInterrupt = 21;
  static constexpr int kAudioMclk = 16;
  static constexpr int kAudioBclk = 9;
  static constexpr int kAudioLrclk = 45;
  static constexpr int kAudioDout = 8;
  static constexpr int kAudioDin = 10;
  static constexpr int kAmplifierEnable = 46;
  static constexpr int kSdCmd = 1;
  static constexpr int kSdClock = 2;
  static constexpr int kSdD0 = 3;
  static constexpr int kUsbDm = 19;
  static constexpr int kUsbDp = 20;
  static constexpr int kBoot = 0;
};

struct BoardStatus {
  bool display_ready = false;
  bool touch_ready = false;
  bool rtc_ready = false;
  bool sd_present = false;
  bool pmu_ready = false;
  bool physical_v1_audited = false;
  const char* reason = "not initialized";
};

struct TouchPoint {
  bool touched = false;
  std::uint8_t fingers = 0U;
  std::uint16_t x = 0U;
  std::uint16_t y = 0U;
};

struct RtcReading {
  bool valid = false;
  bool oscillator_stopped = false;
  std::uint16_t year = 0U;
  std::uint8_t month = 0U;
  std::uint8_t day = 0U;
  std::uint8_t hour = 0U;
  std::uint8_t minute = 0U;
  std::uint8_t second = 0U;
  std::int64_t utc_ms = 0;
};

class BoardV1 {
 public:
  BoardV1() = default;
  ~BoardV1();

  BoardV1(const BoardV1&) = delete;
  BoardV1& operator=(const BoardV1&) = delete;

  Status initialize();
  const BoardStatus& status() const { return status_; }
  bool boot_pressed() const;
  Status read_touch(TouchPoint* point) const;
  Status read_rtc(RtcReading* reading) const;

  // Opaque handles keep the board header usable by the host build while
  // allowing the future LVGL adapter to own the panel presentation path.
  void* display_panel() const;
  void* display_io() const;

  struct HardwareHandles;

 private:
  BoardStatus status_;
  HardwareHandles* handles_ = nullptr;
};

}  // namespace trainer_loop::board_v1
