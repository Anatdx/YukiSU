#pragma once

#include <string>

namespace ksud {

int sepolicy_live_patch(const std::string& policy);
int sepolicy_apply_file(const std::string& file);
int sepolicy_check_rule(const std::string& policy);

}  // namespace ksud
