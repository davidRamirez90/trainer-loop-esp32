#include "provisioning.h"
#include "platform_services.h"

namespace trainer_loop {

Status ProvisioningServer::start(std::uint64_t now_ms) {
  if (session_ == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "setup session is missing");
  if (running_) return Status::ok();
  credentials_ = make_setup_credentials(0x51a7c0deU,
      now_ms == 0U ? 1U : now_ms);
  const Status opened = session_->begin(credentials_, "host-simulator-csrf-token-v1");
  if (!opened) return opened;
  running_ = true;
  return Status::ok();
}

Status ProvisioningServer::host_submit_candidate(const SetupCandidate& candidate) {
  if (!running_ || session_ == nullptr || !session_->active(HostPlatformServices::instance().monotonic_ms()))
    return Status::failure(ErrorCode::kConflict, "simulated setup session is not active");
  if (!commit_handler_) return Status::failure(ErrorCode::kConflict, "setup commit handler is not installed");
  const Status committed = commit_handler_(candidate);
  if (!committed) return committed;
  return stop();
}

Status ProvisioningServer::stop() {
  if (!running_) return Status::ok();
  running_ = false;
  if (session_ != nullptr) session_->close();
  if (stop_handler_) stop_handler_();
  return Status::ok();
}

ProvisioningServer::~ProvisioningServer() { (void)stop(); }

}  // namespace trainer_loop
