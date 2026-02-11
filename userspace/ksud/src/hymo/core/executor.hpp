// core/executor.hpp - Mount execution
#pragma once

#include "../conf/config.hpp"
#include "planner.hpp"
#include <string>
#include <vector>

namespace hymo {

struct ExecutionResult {
  std::vector<std::string> overlay_module_ids;
  std::vector<std::string> magic_module_ids;
};

ExecutionResult execute_plan(const MountPlan& plan, const Config& config, bool hymofs_active);

} // namespace hymo
