#include "restorecon.hpp"
#include <sys/xattr.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <vector>
#include "../defs.hpp"
#include "../log.hpp"

namespace fs = std::filesystem;

namespace ksud {

static constexpr const char* SELINUX_XATTR = "security.selinux";

bool lsetfilecon(const fs::path& path, const std::string& con) {
    const int ret = lsetxattr(path.c_str(), SELINUX_XATTR, con.c_str(), con.length() + 1, 0);
    if (ret != 0) {
        LOGW("Failed to set SELinux context for %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

std::string lgetfilecon(const fs::path& path) {
    std::array<char, 256> buf{};
    const ssize_t len = lgetxattr(path.c_str(), SELINUX_XATTR, buf.data(), buf.size() - 1);
    if (len < 0) {
        return "";
    }
    buf[static_cast<size_t>(len)] = '\0';
    return {buf.data()};
}

bool setsyscon(const fs::path& path) {
    return lsetfilecon(path, SYSTEM_CON);
}

bool restore_syscon(const fs::path& dir) {
    if (!fs::exists(dir)) {
        return true;
    }

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator() && !ec; it.increment(ec)) {
        if (!setsyscon(it->path())) {
            LOGW("Failed to restore context for %s", it->path().c_str());
        }
    }
    if (ec) {
        LOGE("Error walking directory %s: %s", dir.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

bool restore_syscon_if_unlabeled(const fs::path& dir) {
    if (!fs::exists(dir)) {
        return true;
    }

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator() && !ec; it.increment(ec)) {
        const std::string con = lgetfilecon(it->path());
        if (con.empty() || con == UNLABEL_CON) {
            if (!lsetfilecon(it->path(), SYSTEM_CON)) {
                LOGW("Failed to restore context for %s", it->path().c_str());
            }
        }
    }
    if (ec) {
        LOGE("Error walking directory %s: %s", dir.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

bool restorecon() {
    bool success = true;

    // Set context for daemon
    if (!lsetfilecon(DAEMON_PATH, ADB_CON)) {
        LOGW("Failed to set context for daemon");
        success = false;
    }

    // Restore module directory contexts
    if (!restore_syscon_if_unlabeled(MODULE_DIR)) {
        LOGW("Failed to restore contexts for module directory");
        success = false;
    }

    return success;
}

bool restorecon(const fs::path& path, bool recursive) {
    if (!fs::exists(path)) {
        LOGW("Path does not exist: %s", path.c_str());
        return false;
    }

    // For /data/adb, use ADB context
    const char* context = (path == "/data/adb") ? ADB_CON : SYSTEM_CON;

    if (recursive && fs::is_directory(path)) {
        return restore_syscon_if_unlabeled(path);
    } else {
        return lsetfilecon(path, context);
    }
}

}  // namespace ksud
