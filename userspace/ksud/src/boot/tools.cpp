#include "tools.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <climits>
#include <vector>

namespace ksud {

// Find magiskboot binary: always use current process (multi-call ksud embeds magiskboot).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - keep API for callers
std::string find_magiskboot(const std::string& specified_path, const std::string& workdir) {
    (void)specified_path;
    (void)workdir;
    std::array<char, PATH_MAX> self_path{};
    const ssize_t self_len = readlink("/proc/self/exe", self_path.data(), self_path.size() - 1);
    if (self_len <= 0 || static_cast<size_t>(self_len) >= self_path.size()) {
        LOGE("magiskboot (self): readlink /proc/self/exe failed");
        return "";
    }
    self_path[static_cast<size_t>(self_len)] = '\0';
    if (access(self_path.data(), X_OK) != 0) {
        LOGE("magiskboot (self): not executable: %s", self_path.data());
        return "";
    }
    printf("- Using magiskboot: %s (self)\n", self_path.data());
    return {self_path.data()};
}

// DD command wrapper
bool exec_dd(const std::string& input, const std::string& output) {
    auto result = exec_command({"dd", "if=" + input, "of=" + output, "bs=4M", "conv=fsync"});
    if (result.exit_code == 0) {
        return true;
    }

    // Fallback for older toybox/busybox dd variants that may not support conv=fsync.
    auto fallback = exec_command({"dd", "if=" + input, "of=" + output});
    if (fallback.exit_code == 0) {
        return true;
    }

    LOGE("dd failed: if=%s of=%s", input.c_str(), output.c_str());
    if (!result.stderr_str.empty()) {
        LOGE("dd stderr(primary): %s", result.stderr_str.c_str());
    }
    if (!result.stdout_str.empty()) {
        LOGE("dd stdout(primary): %s", result.stdout_str.c_str());
    }
    if (!fallback.stderr_str.empty()) {
        LOGE("dd stderr(fallback): %s", fallback.stderr_str.c_str());
    }
    if (!fallback.stdout_str.empty()) {
        LOGE("dd stdout(fallback): %s", fallback.stdout_str.c_str());
    }
    return false;
}

}  // namespace ksud
