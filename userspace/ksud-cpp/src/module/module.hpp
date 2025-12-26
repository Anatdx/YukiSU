#pragma once

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

} // namespace ksud
