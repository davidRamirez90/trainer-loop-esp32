#pragma once

#include "board_v1.h"
#include "trainer_loop/setup.h"
#include "trainer_loop/ui.h"

namespace trainer_loop {

// The presenter owns the LVGL object tree. It accepts immutable snapshots and
// never performs network, BLE, or filesystem work from a render callback.
class LvglPresenter {
 public:
  Status initialize(void* display_panel, void* display_io, board_v1::BoardV1* board);
  Status show_setup(const SetupCredentials& credentials);
  Status present(const UiSnapshot& snapshot);
  std::optional<UiCommand> pop_command();
  bool initialized() const { return initialized_; }
  struct Runtime;

 private:
  bool initialized_ = false;
  Runtime* runtime_ = nullptr;
  UiCommandQueue commands_;
};

}  // namespace trainer_loop
