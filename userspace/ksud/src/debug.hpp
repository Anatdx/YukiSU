#pragma once

#include <string>
#include <vector>

namespace ksud {

int debug_set_manager(const std::string& pkg);
int debug_get_sign(const std::string& apk);
int debug_mark(const std::vector<std::string>& args);

}  // namespace ksud
