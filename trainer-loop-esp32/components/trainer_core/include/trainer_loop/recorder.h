#pragma once

#include "trainer_loop/journal.h"
#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <cstdint>
#include <vector>

namespace trainer_loop {

class Recorder {
 public:
  explicit Recorder(SessionJournal* journal) : journal_(journal) {}

  Status start(const JournalManifest& manifest, std::uint64_t now_ms);
  Status record(TelemetrySample sample, std::uint64_t now_ms);
  Status transition(const SessionEvent& event);
  Status flush();
  Status finish(const JournalManifest& manifest);
  std::size_t buffered_bytes() const { return buffered_samples_.size() * kTelemetrySampleBytes; }

 private:
  SessionJournal* journal_ = nullptr;
  std::vector<TelemetrySample> buffered_samples_;
  std::uint64_t last_flush_ms_ = 0U;
  bool started_ = false;
};

}  // namespace trainer_loop
