#include "trainer_loop/recorder.h"

#include "trainer_loop/limits.h"

namespace trainer_loop {

Status Recorder::start(const JournalManifest& manifest, std::uint64_t now_ms) {
  if (started_) return Status::failure(ErrorCode::kConflict, "recorder is already started");
  if (journal_ == nullptr) return Status::failure(ErrorCode::kStorage, "recorder journal is missing");
  const Status status = journal_->start(manifest);
  if (!status) return status;
  buffered_samples_.clear();
  buffered_samples_.reserve((kMaxJournalChunkBytes - 20U) / kTelemetrySampleBytes);
  last_flush_ms_ = now_ms;
  started_ = true;
  return Status::ok();
}

Status Recorder::record(TelemetrySample sample, std::uint64_t now_ms) {
  if (!started_) return Status::failure(ErrorCode::kConflict, "recorder is not started");
  const bool flush_interval_elapsed = now_ms >= last_flush_ms_ && now_ms - last_flush_ms_ >= 5000U;
  if (buffered_bytes() + kTelemetrySampleBytes > kMaxJournalChunkBytes || flush_interval_elapsed) {
    const Status flushed = flush();
    if (!flushed) return flushed;
    last_flush_ms_ = now_ms;
  }
  if (buffered_bytes() + kTelemetrySampleBytes > kMaxJournalChunkBytes) {
    return Status::failure(ErrorCode::kStorage, "recording queue is full");
  }
  buffered_samples_.push_back(sample);
  return Status::ok();
}

Status Recorder::transition(const SessionEvent& event) {
  const Status flushed = flush();
  if (!flushed) return flushed;
  return journal_->append_event(event);
}

Status Recorder::flush() {
  if (!started_) return Status::failure(ErrorCode::kConflict, "recorder is not started");
  if (!buffered_samples_.empty()) {
    const Status status = journal_->append_samples(buffered_samples_);
    if (!status) return status;
    buffered_samples_.clear();
  }
  return journal_->flush();
}

Status Recorder::finish(const JournalManifest& manifest) {
  if (!started_) return Status::failure(ErrorCode::kConflict, "recorder is not started");
  const Status flushed = flush();
  if (!flushed) return flushed;
  const Status finalized = journal_->finalize(manifest);
  if (!finalized) return finalized;
  started_ = false;
  return Status::ok();
}

}  // namespace trainer_loop
