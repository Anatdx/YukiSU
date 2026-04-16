#pragma once

#include <string>

namespace ksud::kernelsu_loader {

bool load_module(const char* path, const std::string& param_values = "");

}  // namespace ksud::kernelsu_loader
