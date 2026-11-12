#pragma once

#include "board_v1.h"
#include "trainer_loop/setup.h"
#include "trainer_loop/ui.h"

#if defined(TRAINER_LOOP_HOST_SIM)
#include <vector>
#include <cstdint>
#endif

namespace trainer_loop {

// The presenter owns the LVGL object tree. It accepts immutable snapshots and
// never performs network, BLE, or filesystem work from a render callback.
class LvglPresenter {
 public:
#if defined(TRAINER_LOOP_HOST_SIM)
  ~LvglPresenter();
#endif
  Status initialize(void* display_panel, void* display_io, board_v1::BoardV1* board);
#if defined(TRAINER_LOOP_HOST_SIM)
  struct HostControl {
    std::string id;
    std::string text;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool enabled = true;
  };
  Status initialize_host();
  void host_pump(std::uint32_t delta_ms = 16U);
  void host_pointer(int x, int y, bool pressed);
  const std::uint16_t* host_framebuffer() const;
  Status host_screenshot_png(const std::string& path) const;
  std::vector<HostControl> host_controls() const;
#endif
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
