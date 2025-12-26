#include "susfs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <sys/syscall.h>
#include <unistd.h>

namespace ksud {

// SUSFS syscall constants
constexpr long SUSFS_SYSCALL_NUM = 462;

enum SusfsCmd {
    CMD_SUSFS_GET_VERSION = 0x60000,
    CMD_SUSFS_GET_STATUS = 0x60001,
    CMD_SUSFS_GET_FEATURES = 0x60002,
};

static long susfs_syscall(int cmd, void* arg) {
    return syscall(SUSFS_SYSCALL_NUM, cmd, arg);
}

std::string susfs_get_status() {
    char buf[64] = {0};
    long ret = susfs_syscall(CMD_SUSFS_GET_STATUS, buf);
    if (ret < 0) {
        return "Not available";
    }
    return std::string(buf);
}

std::string susfs_get_version() {
    char buf[32] = {0};
    long ret = susfs_syscall(CMD_SUSFS_GET_VERSION, buf);
    if (ret < 0) {
        return "Unknown";
    }
    return std::string(buf);
}

std::string susfs_get_features() {
    char buf[256] = {0};
    long ret = susfs_syscall(CMD_SUSFS_GET_FEATURES, buf);
    if (ret < 0) {
        return "None";
    }
    return std::string(buf);
}

} // namespace ksud
