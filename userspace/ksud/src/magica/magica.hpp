#pragma once

#include <cstdint>

namespace ksud::magica {

int run(uint16_t port, bool allow_shell = false);
int disable_adb_root();

}  // namespace ksud::magica
