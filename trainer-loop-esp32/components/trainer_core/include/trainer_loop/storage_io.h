#pragma once
#include <cstdio>
#ifndef ESP_PLATFORM
#include <algorithm>
#include <cerrno>
#include <optional>
#endif
namespace trainer_loop {
#ifndef ESP_PLATFORM
// Deterministic host-only ENOSPC/short-write seam; absent from firmware.
inline thread_local std::optional<std::size_t> storage_test_write_budget;
#endif
inline std::size_t storage_write(const void* bytes, std::size_t size, std::FILE* file) {
#ifndef ESP_PLATFORM
  if (storage_test_write_budget) {
    const auto allowed = std::min(size, *storage_test_write_budget);
    *storage_test_write_budget -= allowed;
    const auto written = std::fwrite(bytes, 1, allowed, file);
    if (allowed < size) errno = ENOSPC;
    return written;
  }
#endif
  return std::fwrite(bytes, 1, size, file);
}
} // namespace trainer_loop
