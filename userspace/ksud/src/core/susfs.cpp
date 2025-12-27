#include "susfs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <sys/syscall.h>
#include <unistd.h>
#include <cstring>

namespace ksud {

// SUSFS constants - communicate via reboot syscall like KSU
constexpr uint32_t KSU_INSTALL_MAGIC1 = 0xDEADBEEF;
constexpr uint32_t SUSFS_MAGIC = 0xFAFAFAFA;
constexpr uint32_t CMD_SUSFS_SHOW_VERSION = 0x555e1;
constexpr uint32_t CMD_SUSFS_SHOW_ENABLED_FEATURES = 0x555e2;
constexpr size_t SUSFS_MAX_VERSION_BUFSIZE = 16;
constexpr size_t SUSFS_ENABLED_FEATURES_SIZE = 8192;
constexpr int32_t ERR_CMD_NOT_SUPPORTED = 126;

#pragma pack(push, 1)
struct SusfsVersion {
    char susfs_version[SUSFS_MAX_VERSION_BUFSIZE];
    int32_t err;
};

struct SusfsFeatures {
    char enabled_features[SUSFS_ENABLED_FEATURES_SIZE];
    int32_t err;
};
#pragma pack(pop)

std::string susfs_get_version() {
    SusfsVersion cmd{};
    cmd.err = ERR_CMD_NOT_SUPPORTED;

    long ret = syscall(SYS_reboot, KSU_INSTALL_MAGIC1, SUSFS_MAGIC, CMD_SUSFS_SHOW_VERSION, &cmd);

    if (ret < 0) {
        return "unsupport";
    }

    // Find null terminator
    size_t len = strnlen(cmd.susfs_version, SUSFS_MAX_VERSION_BUFSIZE);
    return std::string(cmd.susfs_version, len);
}

std::string susfs_get_status() {
    std::string version = susfs_get_version();
    // Manager App expects "true" or "false" string
    if (version == "unsupport") {
        return "false";
    }
    return "true";
}

std::string susfs_get_features() {
    SusfsFeatures cmd{};
    cmd.err = ERR_CMD_NOT_SUPPORTED;

    long ret =
        syscall(SYS_reboot, KSU_INSTALL_MAGIC1, SUSFS_MAGIC, CMD_SUSFS_SHOW_ENABLED_FEATURES, &cmd);

    if (ret < 0) {
        return "None";
    }

    size_t len = strnlen(cmd.enabled_features, SUSFS_ENABLED_FEATURES_SIZE);
    return std::string(cmd.enabled_features, len);
}

}  // namespace ksud
