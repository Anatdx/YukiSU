// core/executor.hpp - Mount execution
#pragma once

#include <string>
#include <vector>
#include "../conf/config.hpp"
#include "planner.hpp"

namespace hymo {

struct ExecutionResult {
    std::vector<std::string> overlay_module_ids;
    std::vector<std::string> magic_module_ids;
};

ExecutionResult execute_plan(const MountPlan& plan, const Config& config);

}  // namespace hymo
