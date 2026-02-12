#include "../assets.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ksud {

static const std::vector<std::string>& empty_assets() {
    static const std::vector<std::string> empty;
    return empty;
}

const std::vector<std::string>& list_assets() {
    return empty_assets();
}

bool get_asset(const std::string& /*name*/, const uint8_t*& /*data*/, size_t& /*size*/) {
    return false;
}

bool copy_asset_to_file(const std::string& /*name*/, const std::string& /*dest_path*/) {
    return false;
}

std::vector<std::string> list_supported_kmi() {
    return {};
}

int ensure_binaries(bool /*ignore_if_exist*/) {
    if (!ensure_dir_exists(BINARY_DIR)) {
        LOGE("Failed to create binary directory: %s", BINARY_DIR);
        return 1;
    }

    struct stat st;
    if (stat(DAEMON_PATH, &st) == 0 && stat(DAEMON_LINK_PATH, &st) != 0) {
        unlink(DAEMON_LINK_PATH);
        if (symlink(DAEMON_PATH, DAEMON_LINK_PATH) != 0) {
            LOGW("Failed to create ksud symlink: %s", strerror(errno));
        } else {
            LOGI("Created ksud symlink: %s -> %s", DAEMON_LINK_PATH, DAEMON_PATH);
        }
    }
#if defined(NDK_BUSYBOX_AVAILABLE) && NDK_BUSYBOX_AVAILABLE
    std::string busybox_link = std::string(BINARY_DIR) + "busybox";
    if (stat(busybox_link.c_str(), &st) != 0) {
        unlink(busybox_link.c_str());
        if (symlink(DAEMON_PATH, busybox_link.c_str()) != 0) {
            LOGW("Failed to create busybox symlink: %s", strerror(errno));
        } else {
            LOGI("Created busybox symlink: %s -> %s", busybox_link.c_str(), DAEMON_PATH);
        }
    }
#endif
    return 0;
}

}  // namespace ksud
