#include "wifi_station.h"
#include "platform_services.h"

namespace trainer_loop {

bool WifiStation::connected() const {
  return connected_ && HostPlatformServices::instance().wifi_available();
}

WifiStation::~WifiStation() = default;

Status WifiStation::initialize() {
  initialized_ = true;
  return Status::ok();
}

Status WifiStation::connect(const std::string& ssid, const std::string& password) {
  if (!initialized_) return Status::failure(ErrorCode::kConflict, "Wi-Fi is not initialized");
  if (ssid.empty() || ssid.size() > 32U || password.size() > 64U)
    return Status::failure(ErrorCode::kBounds, "Wi-Fi credentials are invalid");
  connected_ = HostPlatformServices::instance().wifi_available();
  HostPlatformServices::instance().record_event(connected_ ? "wifi.connected" : "wifi.failed");
  return connected_ ? Status::ok()
                    : Status::failure(ErrorCode::kTransport, "simulated Wi-Fi network is unavailable");
}

Status WifiStation::verify_credentials(const std::string& ssid, const std::string& password,
                                      std::uint32_t timeout_ms) {
  if (timeout_ms == 0U) return Status::failure(ErrorCode::kTimeout, "Wi-Fi verification timeout is zero");
  const Status started = connect(ssid, password);
  if (!started) return started;
  return wait_for_connection(timeout_ms);
}

Status WifiStation::wait_for_connection(std::uint32_t timeout_ms) {
  if (!initialized_) return Status::failure(ErrorCode::kConflict, "Wi-Fi is not initialized");
  if (connected()) return Status::ok();
  return timeout_ms == 0U
      ? Status::failure(ErrorCode::kTimeout, "Wi-Fi association timed out")
      : Status::failure(ErrorCode::kTransport, "simulated Wi-Fi network is unavailable");
}

Status WifiStation::disconnect() {
  connected_ = false;
  HostPlatformServices::instance().record_event("wifi.disconnected");
  return Status::ok();
}

}  // namespace trainer_loop
