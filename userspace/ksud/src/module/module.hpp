#pragma once

#include <map>
#include <string>
#include <vector>

namespace ksud {

// Module management
int module_install(const std::string& zip_path);
int module_uninstall(const std::string& id);
int module_undo_uninstall(const std::string& id);
int module_enable(const std::string& id);
int module_disable(const std::string& id);
int module_run_action(const std::string& id);
int module_list();

// Internal functions
int uninstall_all_modules();
int prune_modules();
int disable_all_modules();
int handle_updated_modules();

// Script execution
int exec_stage_script(const std::string& stage, bool block);
int exec_common_scripts(const std::string& stage_dir, bool block);
int load_sepolicy_rule();
int load_system_prop();

// Get all managed features from active modules
// Modules declare managed features via config system (manage.<feature>=true)
// Returns: map<ModuleId, vector<ManagedFeature>>
std::map<std::string, std::vector<std::string>> get_managed_features();

}  // namespace ksud
