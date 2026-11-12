#pragma once

#include "trainer_loop/ui.h"

#include <string>
#include <vector>

namespace trainer_loop {

const std::vector<std::string>& ui_fixture_names();
UiSnapshot ui_fixture(const std::string& name);

}  // namespace trainer_loop
