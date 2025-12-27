#include "restorecon.hpp"
#include "log.hpp"

#include <sys/xattr.h>
#include <cstring>

namespace ksud {

int lsetfilecon(const std::string& path, const std::string& context) {
    int ret = lsetxattr(path.c_str(), "security.selinux", context.c_str(), context.length() + 1, 0);
    if (ret < 0) {
        LOGW("Failed to set SELinux context on %s: %s", path.c_str(), strerror(errno));
        return -1;
    }
    return 0;
}

int restorecon(const std::string& path, bool recursive) {
    // For recursive restorecon, we'd need to walk the directory
    // For now, just handle the single file case
    return lsetfilecon(path, ADB_CON);
}

}  // namespace ksud
