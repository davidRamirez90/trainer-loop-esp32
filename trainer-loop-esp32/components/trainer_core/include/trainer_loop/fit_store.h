#pragma once

#include "trainer_loop/result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace trainer_loop {

struct FitArtifact {
  std::string session_uuid;
  std::string path;
  std::string sha256;
  std::size_t size = 0U;
};

class FitArtifactStore {
 public:
  explicit FitArtifactStore(std::string directory);

  Status initialize();
  Result<FitArtifact> publish(const std::string& session_uuid,
                              const std::vector<std::uint8_t>& bytes);
  Result<std::vector<std::uint8_t>> read(const FitArtifact& artifact) const;

 private:
  std::string path_for(const std::string& session_uuid) const;
  std::string directory_;
};

}  // namespace trainer_loop
