#pragma once

#include <string>

namespace ksud {

int profile_get_sepolicy(const std::string& package);
int profile_set_sepolicy(const std::string& package, const std::string& policy);
int profile_get_template(const std::string& id);
int profile_set_template(const std::string& id, const std::string& template_str);
int profile_delete_template(const std::string& id);
int profile_list_templates();

// Apply all profile sepolicies
int apply_profile_sepolies();

}  // namespace ksud
