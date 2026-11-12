#pragma once

#include <cstdint>
#include <string>

namespace trainer_loop {
bool lvgl_host_write_png(const std::string& path, const std::uint16_t* pixels,
                         int width, int height);
}
