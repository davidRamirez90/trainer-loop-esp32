#pragma once

#include "trainer_loop/result.h"

#include <cstddef>
#include <cstdint>

namespace trainer_loop {

constexpr bool storage_mount_format_allowed(bool partition_is_erased) noexcept {
  return partition_is_erased;
}

struct StorageStatus {
  bool mounted = false;
  std::uint64_t total_bytes = 0U;
  std::uint64_t used_bytes = 0U;
};

class StorageMount {
 public:
  ~StorageMount();

  Status mount();
  Status unmount();
  const StorageStatus& status() const { return status_; }
  bool mounted() const { return status_.mounted; }

 private:
  StorageStatus status_;
};

}  // namespace trainer_loop
