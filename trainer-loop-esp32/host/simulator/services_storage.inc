#include "storage_mount.h"
#include "platform_services.h"

#include <filesystem>

namespace trainer_loop {

StorageMount::~StorageMount() { (void)unmount(); }

Status StorageMount::mount() {
  if (status_.mounted) return Status::ok();
  std::error_code error;
  const auto& root = HostPlatformServices::instance().root();
  std::filesystem::create_directories(root, error);
  if (error) return Status::failure(ErrorCode::kStorage, "host storage root could not be created: " + error.message());
  std::uint64_t used = 0U;
  std::size_t entry_count = 0U;
  for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end && entry_count < 4096U; iterator.increment(error), ++entry_count) {
    if (iterator->is_regular_file(error)) {
      const auto bytes = iterator->file_size(error);
      if (bytes > 64ULL * 1024ULL * 1024ULL - used)
        return Status::failure(ErrorCode::kStorage, "host storage exceeds simulated capacity");
      used += bytes;
    }
  }
  if (!error && entry_count >= 4096U)
    return Status::failure(ErrorCode::kBounds, "host storage contains too many entries");
  if (error) return Status::failure(ErrorCode::kStorage, "host storage usage could not be read: " + error.message());
  status_.mounted = true;
  status_.total_bytes = 64ULL * 1024ULL * 1024ULL;
  status_.used_bytes = used;
  return Status::ok();
}

Status StorageMount::unmount() {
  status_ = StorageStatus{};
  return Status::ok();
}

}  // namespace trainer_loop
