#pragma once

#include <map>
#include <string>
#include <vector>

namespace ksud {

struct CommonScriptEnv {
    std::string kernel_ver_code;
    std::string path;
    bool late_load{};
    bool zygisk_enabled{};
};

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
int regenerate_preinit_rc();

// Script execution
int exec_stage_script(const std::string& stage, bool block);
int exec_common_scripts(const std::string& stage_dir, bool block);
int load_sepolicy_rule();
int load_system_prop();

// Get all managed features from active modules
// Modules declare managed features via config system (manage.<feature>=true)
// Returns: map<ModuleId, vector<ManagedFeature>>
std::map<std::string, std::vector<std::string>> get_managed_features();

// Metamodule
std::string get_metamodule_id();

// Shared script environment
CommonScriptEnv build_common_script_env();
void apply_common_script_env(const CommonScriptEnv& env, const char* module_id = nullptr,
                             bool set_magisk_compat = false);

}  // namespace ksud
