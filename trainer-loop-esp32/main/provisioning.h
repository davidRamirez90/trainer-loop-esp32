#pragma once

#include "trainer_loop/setup.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace trainer_loop {

class ProvisioningServer {
 public:
  using CommitHandler = std::function<Status(const SetupCandidate&)>;
  using StopHandler = std::function<void()>;

  explicit ProvisioningServer(SetupSession* session) : session_(session) {}
  ~ProvisioningServer();

  void set_commit_handler(CommitHandler handler) { commit_handler_ = std::move(handler); }
  void set_stop_handler(StopHandler handler) { stop_handler_ = std::move(handler); }
  Status start(std::uint64_t now_ms);
  Status stop();
#ifdef ESP_PLATFORM
  void request_stop();
#else
  // Deterministic simulator input for the same validated setup commit path.
  Status host_submit_candidate(const SetupCandidate& candidate);
#endif
  bool running() const { return running_; }
  const SetupCredentials& credentials() const { return credentials_; }
  SetupSession* session() const { return session_; }
  const CommitHandler& commit_handler() const { return commit_handler_; }
#ifdef ESP_PLATFORM
  void* http_server() const { return http_server_; }
#endif

 private:
  SetupSession* session_ = nullptr;
  CommitHandler commit_handler_;
  StopHandler stop_handler_;
  SetupCredentials credentials_;
  std::atomic<bool> running_{false};
#ifdef ESP_PLATFORM
  static void event_handler(void* argument, esp_event_base_t event_base,
                            std::int32_t event_id, void* event_data);
  static void stop_task_entry(void* argument);
  esp_err_t ensure_setup_network();
  void* http_server_ = nullptr;
  void* ap_netif_ = nullptr;
  esp_event_handler_instance_t wifi_event_instance_ = nullptr;
  esp_event_handler_instance_t ip_event_instance_ = nullptr;
  esp_timer_handle_t expiry_timer_ = nullptr;
  std::atomic<bool> stop_requested_{false};
#endif
};

}  // namespace trainer_loop
