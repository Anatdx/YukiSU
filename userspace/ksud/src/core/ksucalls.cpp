#include "ksucalls.hpp"
#include "../defs.hpp"
#include "../log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ksud {

// Magic constants
// NOTE: Avoid 0xDEAD/0xBEEF patterns - easily detected by root checkers
constexpr uint32_t KSU_INSTALL_MAGIC1 = 0xDEADBEEF;
constexpr uint32_t KSU_INSTALL_MAGIC2 = 0xCAFEBABE;
constexpr int KSU_PRCTL_GET_FD = static_cast<int>(0x59554B4AU);  // "YUKJ" in hex

namespace {

struct PrctlGetFdCmd {
    int32_t result;
    int32_t fd;
};

int g_driver_fd = -1;
bool g_driver_fd_init = false;
GetInfoCmd g_info_cache = {0, 0};
bool g_info_cached = false;

constexpr size_t kLinkPathSize = 64;
constexpr size_t kReadlinkBufSize = 256;

auto scan_driver_fd() -> int {
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }

    int found_fd = -1;
    std::array<char, kLinkPathSize> link_path{};
    std::array<char, kReadlinkBufSize> target{};

    // readdir is not thread-safe; we use it only during single-threaded init.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    while (struct dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char* end = nullptr;
        const long fd_num = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || fd_num < 0) {
            continue;
        }

        const int snprintf_ret =
            snprintf(link_path.data(), link_path.size(), "/proc/self/fd/%ld", fd_num);
        if (snprintf_ret <= 0 || static_cast<size_t>(snprintf_ret) >= link_path.size()) {
            continue;
        }

        const ssize_t len = readlink(link_path.data(), target.data(), target.size() - 1);
        if (len > 0 && static_cast<size_t>(len) < target.size()) {
            target[static_cast<size_t>(len)] = '\0';
            if (strstr(target.data(), "[ksu_driver]") != nullptr) {
                found_fd = static_cast<int>(fd_num);
                break;
            }
        }
    }

    closedir(dir);
    return found_fd;
}

auto init_driver_fd() -> int {
    // Method 1: Check if we already have an inherited fd
    const int driver_fd = scan_driver_fd();
    if (driver_fd >= 0) {
        LOGD("Found inherited driver fd: %d", driver_fd);
        return driver_fd;
    }

    // Method 2: Try prctl to get fd (SECCOMP-safe)
    PrctlGetFdCmd prctl_cmd = {-1, -1};
    prctl(KSU_PRCTL_GET_FD, &prctl_cmd, 0, 0, 0);
    if (prctl_cmd.result == 0 && prctl_cmd.fd >= 0) {
        LOGD("Got driver fd via prctl: %d", prctl_cmd.fd);
        return prctl_cmd.fd;
    }

    // Method 3: Fallback to reboot syscall (may be blocked by SECCOMP)
    int fd_reboot = -1;
    syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd_reboot);
    if (fd_reboot >= 0) {
        LOGD("Got driver fd via reboot syscall: %d", fd_reboot);
        return fd_reboot;
    }

    LOGE("Failed to get driver fd");
    return -1;
}

auto get_driver_fd() -> int {
    if (!g_driver_fd_init) {
        g_driver_fd = init_driver_fd();
        g_driver_fd_init = true;
    }
    return g_driver_fd;
}

}  // namespace

int ksuctl(int request, void* arg) {
    const int fd = get_driver_fd();
    if (fd < 0) {
        return -1;
    }

    const int ret = ioctl(fd, request, arg);
    if (ret < 0) {
        LOGE("ioctl failed: request=0x%x, errno=%d (%s)", request, errno, strerror(errno));
        return -1;
    }

    return ret;
}

namespace {

const GetInfoCmd& get_info() {
    if (!g_info_cached) {
        GetInfoCmd cmd = {0, 0};
        ksuctl(KSU_IOCTL_GET_INFO, &cmd);
        g_info_cache = cmd;
        g_info_cached = true;
    }
    return g_info_cache;
}

void report_event(uint32_t event) {
    ReportEventCmd cmd = {event};
    ksuctl(KSU_IOCTL_REPORT_EVENT, &cmd);
}

}  // namespace

int32_t get_version() {
    return static_cast<int32_t>(get_info().version);
}

uint32_t get_flags() {
    return get_info().flags;
}

int grant_root() {
    return ksuctl(KSU_IOCTL_GRANT_ROOT, nullptr);
}

void report_post_fs_data() {
    report_event(EVENT_POST_FS_DATA);
}

void report_boot_complete() {
    report_event(EVENT_BOOT_COMPLETED);
}

void report_module_mounted() {
    report_event(EVENT_MODULE_MOUNTED);
}

bool check_kernel_safemode() {
    CheckSafemodeCmd cmd = {0};
    ksuctl(KSU_IOCTL_CHECK_SAFEMODE, &cmd);
    return cmd.in_safe_mode != 0;
}

int set_sepolicy(const SetSepolicyCmd& cmd) {
    SetSepolicyCmd ioctl_cmd = cmd;
    return ksuctl(KSU_IOCTL_SET_SEPOLICY, &ioctl_cmd);
}

std::pair<uint64_t, bool> get_feature(uint32_t feature_id) {
    GetFeatureCmd cmd = {feature_id, 0, 0};
    const int ret = ksuctl(KSU_IOCTL_GET_FEATURE, &cmd);
    if (ret < 0) {
        return {0, false};
    }
    return {cmd.value, cmd.supported != 0};
}

int set_feature(uint32_t feature_id, uint64_t value) {
    SetFeatureCmd cmd = {feature_id, value};
    return ksuctl(KSU_IOCTL_SET_FEATURE, &cmd);
}

int get_wrapped_fd(int fd) {
    GetWrapperFdCmd cmd = {fd, 0};
    return ksuctl(KSU_IOCTL_GET_WRAPPER_FD, &cmd);
}

uint32_t mark_get(int32_t pid) {
    ManageMarkCmd cmd = {KSU_MARK_GET, pid, 0};
    ksuctl(KSU_IOCTL_MANAGE_MARK, &cmd);
    return cmd.result;
}

int mark_set(int32_t pid) {
    ManageMarkCmd cmd = {KSU_MARK_MARK, pid, 0};
    return ksuctl(KSU_IOCTL_MANAGE_MARK, &cmd);
}

int mark_unset(int32_t pid) {
    ManageMarkCmd cmd = {KSU_MARK_UNMARK, pid, 0};
    return ksuctl(KSU_IOCTL_MANAGE_MARK, &cmd);
}

int mark_refresh() {
    ManageMarkCmd cmd = {KSU_MARK_REFRESH, 0, 0};
    return ksuctl(KSU_IOCTL_MANAGE_MARK, &cmd);
}

int nuke_ext4_sysfs(const std::string& mnt) {
    NukeExt4SysfsCmd cmd = {reinterpret_cast<uint64_t>(mnt.c_str())};
    return ksuctl(KSU_IOCTL_NUKE_EXT4_SYSFS, &cmd);
}

int umount_list_wipe() {
    AddTryUmountCmd cmd = {0, 0, UMOUNT_WIPE};
    return ksuctl(KSU_IOCTL_ADD_TRY_UMOUNT, &cmd);
}

int umount_list_add(const std::string& path, uint32_t flags) {
    AddTryUmountCmd cmd = {reinterpret_cast<uint64_t>(path.c_str()), flags, UMOUNT_ADD};
    return ksuctl(KSU_IOCTL_ADD_TRY_UMOUNT, &cmd);
}

int umount_list_del(const std::string& path) {
    AddTryUmountCmd cmd = {reinterpret_cast<uint64_t>(path.c_str()), 0, UMOUNT_DEL};
    return ksuctl(KSU_IOCTL_ADD_TRY_UMOUNT, &cmd);
}

std::optional<std::string> umount_list_list() {
    constexpr size_t kBufSize = 4096;
    std::array<char, kBufSize> buffer{};

    ListTryUmountCmd cmd = {reinterpret_cast<uint64_t>(buffer.data()),
                            static_cast<uint32_t>(buffer.size())};
    const int ret = ksuctl(KSU_IOCTL_LIST_TRY_UMOUNT, &cmd);
    if (ret < 0) {
        return std::nullopt;
    }

    return std::string(buffer.data());
}

}  // namespace ksud
