#pragma once

#include "trainer_loop/settings.h"

namespace trainer_loop {

class NvsSettingsBackend {
 public:
  Status initialize();
  Status load(SettingsStore* store) const;
  Status save(const SettingsGeneration& settings) const;
  bool encrypted_at_rest() const;
};

}  // namespace trainer_loop
