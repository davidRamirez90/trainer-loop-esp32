#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"

#include <cstddef>
#include <string>
#include <vector>

namespace trainer_loop {

class HistoryStore {
 public:
  explicit HistoryStore(std::string catalog_path);

  Status load();
  Status save(const SessionSummary& summary);
  std::vector<SessionSummary> recent(std::size_t limit = 4U) const;
  Result<SessionSummary> find(const std::string& uuid) const;
  std::size_t size() const { return entries_.size(); }

 private:
  Status persist() const;

  std::string catalog_path_;
  std::vector<SessionSummary> entries_;
};

}  // namespace trainer_loop
