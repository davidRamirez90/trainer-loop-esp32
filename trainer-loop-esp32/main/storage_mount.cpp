#include "storage_mount.h"

#ifdef ESP_PLATFORM

#include "esp_partition.h"
#include "esp_littlefs.h"
#include "esp_log.h"

#include <algorithm>
#include <array>

namespace trainer_loop {

namespace {

constexpr char kTag[] = "trainer_storage";

bool partition_is_erased(const esp_partition_t* partition) {
  if (partition == nullptr) return false;
  std::array<std::uint8_t, 4096U> buffer{};
  for (std::size_t offset = 0U; offset < partition->size; offset += buffer.size()) {
    const std::size_t length = std::min<std::size_t>(buffer.size(), partition->size - offset);
    if (esp_partition_read(partition, offset, buffer.data(), length) != ESP_OK) return false;
    if (std::any_of(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(length),
                    [](std::uint8_t value) { return value != 0xffU; })) {
      return false;
    }
  }
  return true;
}

}  // namespace

Status StorageMount::mount() {
  if (status_.mounted) return Status::ok();
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "storage");
  const bool allow_initial_format = partition != nullptr &&
                                    storage_mount_format_allowed(partition_is_erased(partition));
  esp_vfs_littlefs_conf_t config{};
  config.base_path = "/storage";
  config.partition_label = "storage";
  // Formatting is permitted only for a partition proven to be entirely
  // erased. Any existing bytes make a mount failure a preservation/recovery
  // condition; the application must not destroy unsynced rides to boot.
  config.format_if_mount_failed = allow_initial_format;
  esp_err_t error = esp_vfs_littlefs_register(&config);
  if (error != ESP_OK) {
    if (!allow_initial_format) {
      ESP_LOGE(kTag, "LittleFS mount failed; existing storage preserved for recovery");
      return Status::failure(ErrorCode::kStorage,
                             "internal storage mount failed; existing data preserved for recovery");
    }
    return Status::failure(ErrorCode::kStorage, "empty internal storage could not be initialized");
  }
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
#include "../host/simulator/services_storage.inc"

#endif
