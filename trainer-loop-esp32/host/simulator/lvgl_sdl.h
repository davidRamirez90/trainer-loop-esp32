#pragma once

#include <cstdint>
#include <functional>

namespace trainer_loop {
class LvglPresenter;
int run_lvgl_sdl(LvglPresenter& presenter,
                 const std::function<void(std::uint32_t)>& tick = {},
                 const std::function<void(bool)>& boot_button = {}, int scale = 2,
                 unsigned frame_limit = 0);
}
