#include "storage_mount.h"

#ifdef ESP_PLATFORM

#include "esp_littlefs.h"
#include "esp_log.h"

namespace trainer_loop {

namespace {

constexpr char kTag[] = "trainer_storage";

}  // namespace

Status StorageMount::mount() {
  if (status_.mounted) return Status::ok();
  esp_vfs_littlefs_conf_t config{};
  config.base_path = "/storage";
  config.partition_label = "storage";
  config.format_if_mount_failed = true;
  esp_err_t error = esp_vfs_littlefs_register(&config);
  if (error != ESP_OK) return Status::failure(ErrorCode::kStorage, "internal storage mount failed");
  std::size_t total = 0U;
  std::size_t used = 0U;
  error = esp_littlefs_info("storage", &total, &used);
  if (error != ESP_OK || used > total) {
    esp_vfs_littlefs_unregister("storage");
    return Status::failure(ErrorCode::kStorage, "internal storage usage is unavailable");
  }
  status_.mounted = true;
  status_.total_bytes = static_cast<std::uint64_t>(total);
  status_.used_bytes = static_cast<std::uint64_t>(used);
  ESP_LOGI(kTag, "LittleFS mounted (%u/%u bytes used)", static_cast<unsigned>(used),
           static_cast<unsigned>(total));
  return Status::ok();
}

Status StorageMount::unmount() {
  if (!status_.mounted) return Status::ok();
  esp_vfs_littlefs_unregister("storage");
  status_ = StorageStatus{};
  return Status::ok();
}

StorageMount::~StorageMount() { (void)unmount(); }

}  // namespace trainer_loop

#else

namespace trainer_loop {

StorageMount::~StorageMount() = default;

Status StorageMount::mount() {
  return Status::failure(ErrorCode::kUnsupported, "ESP-IDF storage mount is unavailable in the host build");
}

Status StorageMount::unmount() {
  status_ = StorageStatus{};
  return Status::ok();
}

}  // namespace trainer_loop

#endif
