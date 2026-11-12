#include "time_sync.h"

#ifdef ESP_PLATFORM

#include "esp_netif_sntp.h"

#include <sys/time.h>

namespace trainer_loop {

Status TimeSynchronizer::synchronize(TimeService* time, std::uint32_t timeout_ms) {
  if (time == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "time service is null");
  if (timeout_ms == 0U) return Status::failure(ErrorCode::kTimeout, "SNTP timeout is zero");

  if (!initialized_) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    const esp_err_t error = esp_netif_sntp_init(&config);
    if (error != ESP_OK) return Status::failure(ErrorCode::kTransport, "SNTP initialization failed");
    initialized_ = true;
  } else {
    const esp_err_t error = esp_netif_sntp_start();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
      return Status::failure(ErrorCode::kTransport, "SNTP restart failed");
    }
  }

  const esp_err_t wait_error = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
  if (wait_error != ESP_OK) return Status::failure(ErrorCode::kTimeout, "SNTP did not produce trusted time");
  timeval current{};
  if (::gettimeofday(&current, nullptr) != 0 || current.tv_sec < 0) {
    return Status::failure(ErrorCode::kTimeUntrusted, "system clock could not be read after SNTP");
  }
  const std::int64_t utc_ms = static_cast<std::int64_t>(current.tv_sec) * 1000LL +
                              static_cast<std::int64_t>(current.tv_usec / 1000L);
  return time->set_from_sntp(utc_ms);
}

}  // namespace trainer_loop

#else
#include "../host/simulator/services_time.inc"

#endif
