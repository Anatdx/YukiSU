#include "ksucalls.hpp"
#include "../defs.hpp"
#include "../log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstring>

namespace ksud {

// Global driver fd
static int g_driver_fd = -1;
static bool g_driver_fd_init = false;

// Cached info
static GetInfoCmd g_info_cache = {0, 0};
static bool g_info_cached = false;

// Magic constants
constexpr uint32_t KSU_INSTALL_MAGIC1 = 0xDEADBEEF;
constexpr uint32_t KSU_INSTALL_MAGIC2 = 0xCAFEBABE;
constexpr int KSU_PRCTL_GET_FD = static_cast<int>(0xDEAD5556u);

struct PrctlGetFdCmd {
    int32_t result;
    int32_t fd;
};

static int scan_driver_fd() {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir)
        return -1;

    int found_fd = -1;
    struct dirent* entry;
    char link_path[64];
    char target[256];

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;

        int fd_num = atoi(entry->d_name);
        snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd_num);

        ssize_t len = readlink(link_path, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            if (strstr(target, "[ksu_driver]") != nullptr) {
                found_fd = fd_num;
                break;
            }
        }
    }

    closedir(dir);
    return found_fd;
}

static int init_driver_fd() {
    // Method 1: Check if we already have an inherited fd
    int fd = scan_driver_fd();
    if (fd >= 0) {
        LOGD("Found inherited driver fd: %d", fd);
        return fd;
    }

    // Method 2: Try prctl to get fd (SECCOMP-safe)
    PrctlGetFdCmd prctl_cmd = {-1, -1};
    prctl(KSU_PRCTL_GET_FD, &prctl_cmd, 0, 0, 0);
    if (prctl_cmd.result == 0 && prctl_cmd.fd >= 0) {
        LOGD("Got driver fd via prctl: %d", prctl_cmd.fd);
        return prctl_cmd.fd;
    }

    // Method 3: Fallback to reboot syscall (may be blocked by SECCOMP)
    fd = -1;
    syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd);
    if (fd >= 0) {
        LOGD("Got driver fd via reboot syscall: %d", fd);
        return fd;
    }

    LOGE("Failed to get driver fd");
    return -1;
}

static int get_driver_fd() {
    if (!g_driver_fd_init) {
        g_driver_fd = init_driver_fd();
        g_driver_fd_init = true;
    }
    return g_driver_fd;
}

int ksuctl(int request, void* arg) {
    int fd = get_driver_fd();
    if (fd < 0) {
        return -1;
    }

    int ret = ioctl(fd, request, arg);
    if (ret < 0) {
        LOGE("ioctl failed: request=0x%x, errno=%d (%s)", request, errno, strerror(errno));
        return -1;
    }

    return ret;
}

static const GetInfoCmd& get_info() {
    if (!g_info_cached) {
        GetInfoCmd cmd = {0, 0};
        ksuctl(KSU_IOCTL_GET_INFO, &cmd);
        g_info_cache = cmd;
        g_info_cached = true;
    }
    return g_info_cache;
}

int32_t get_version() {
    return static_cast<int32_t>(get_info().version);
}

uint32_t get_flags() {
    return get_info().flags;
}

int grant_root() {
    return ksuctl(KSU_IOCTL_GRANT_ROOT, nullptr);
}

static void report_event(uint32_t event) {
    ReportEventCmd cmd = {event};
    ksuctl(KSU_IOCTL_REPORT_EVENT, &cmd);
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
    int ret = ksuctl(KSU_IOCTL_GET_FEATURE, &cmd);
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
    constexpr size_t BUF_SIZE = 4096;
    char buffer[BUF_SIZE] = {0};

    ListTryUmountCmd cmd = {reinterpret_cast<uint64_t>(buffer), BUF_SIZE};
    int ret = ksuctl(KSU_IOCTL_LIST_TRY_UMOUNT, &cmd);
    if (ret < 0) {
        return std::nullopt;
    }

    return std::string(buffer);
}

}  // namespace ksud
