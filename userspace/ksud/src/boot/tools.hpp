#pragma once

#include <string>

namespace ksud {

// Find magiskboot binary
// If specified_path is provided, checks it.
// Otherwise, checks standard locations and PATH.
std::string find_magiskboot(const std::string& specified_path = "",
                            const std::string& workdir = "");

// Simple DD command wrapper
bool exec_dd(const std::string& input, const std::string& output);

}  // namespace ksud
