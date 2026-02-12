#pragma once

#include <string>

namespace ksud {

// Ensure BINARY_DIR exists and symlinks (ksud, busybox) are created. Returns 0 on success.
int ensure_binaries(bool ignore_if_exist);

}  // namespace ksud
