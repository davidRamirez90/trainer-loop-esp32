#include "time_sync.h"
#include "platform_services.h"

namespace trainer_loop {

Status TimeSynchronizer::synchronize(TimeService* time, std::uint32_t timeout_ms) {
  if (time == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "time service is null");
  if (timeout_ms == 0U) return Status::failure(ErrorCode::kTimeout, "SNTP timeout is zero");
  auto& services = HostPlatformServices::instance();
  if (!services.wifi_available()) {
    return Status::failure(ErrorCode::kTimeout, "simulated SNTP is unavailable");
  }
  if (!services.time_sync_available()) {
    services.record_event("time.sync.unavailable");
    return Status::failure(ErrorCode::kTimeUntrusted, "simulated SNTP is unavailable");
  }
  const std::int64_t utc = services.has_utc() ? services.utc_ms() : 1767225600000LL;
  initialized_ = true;
  const Status synchronized = time->set_from_sntp(utc);
  if (synchronized) services.record_event("time.sync.ok");
  return synchronized;
}

}  // namespace trainer_loop
