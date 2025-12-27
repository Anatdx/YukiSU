#pragma once

#include <string>

namespace ksud {

// Metamodule support
int metamodule_init();
int metamodule_exec_stage_script(const std::string& stage, bool block);
int metamodule_exec_mount_script();

}  // namespace ksud
