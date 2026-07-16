#pragma once

#include <string>

namespace ksud {

// Metamodule support
int metamodule_init();
int metamodule_exec_stage_script(const std::string& stage, bool block);
int metamodule_exec_mount_script();
int metamodule_exec_uninstall_script(const std::string& module_id);

}  // namespace ksud
