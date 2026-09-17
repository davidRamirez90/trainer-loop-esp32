#pragma once

#include "trainer_loop/result.h"

#include <cstdint>
#include <string>

#ifdef ESP_PLATFORM
#include "esp_event.h"
#endif

namespace trainer_loop {

class WifiStation {
 public:
  ~WifiStation();

  Status initialize();
  Status connect(const std::string& ssid, const std::string& password);
  Status verify_credentials(const std::string& ssid, const std::string& password,
                            std::uint32_t timeout_ms);
  Status wait_for_connection(std::uint32_t timeout_ms);
  Status disconnect();
  bool connected() const { return connected_; }

 private:
#ifdef ESP_PLATFORM
  static void event_handler(void* argument, esp_event_base_t event_base,
                            std::int32_t event_id, void* event_data);
  bool event_handlers_registered_ = false;
#endif
  bool initialized_ = false;
  bool connected_ = false;
};

}  // namespace trainer_loop
