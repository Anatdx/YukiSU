#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace ksud {

// ioctl macros - cast to unsigned to avoid overflow warnings
// Note: size field is 0 to match kernel _IOC(..., 0) definitions
#define _IOC(dir, type, nr, size) \
    (static_cast<uint32_t>(((dir) << 30) | ((type) << 8) | (nr) | ((size) << 16)))
#define _IO(type, nr) _IOC(0, type, nr, 0)
#define _IOR(type, nr, sz) _IOC(2, type, nr, 0)
#define _IOW(type, nr, sz) _IOC(1, type, nr, 0)
#define _IOWR(type, nr, sz) _IOC(3, type, nr, 0)

constexpr uint32_t K = 'K';

// ioctl commands
constexpr uint32_t KSU_IOCTL_GRANT_ROOT = _IO(K, 1);
constexpr uint32_t KSU_IOCTL_GET_INFO = _IOR(K, 2, uint64_t);
constexpr uint32_t KSU_IOCTL_REPORT_EVENT = _IOW(K, 3, uint64_t);
constexpr uint32_t KSU_IOCTL_SET_SEPOLICY = _IOWR(K, 4, uint64_t);
constexpr uint32_t KSU_IOCTL_CHECK_SAFEMODE = _IOR(K, 5, uint64_t);
constexpr uint32_t KSU_IOCTL_GET_FEATURE = _IOWR(K, 13, uint64_t);
constexpr uint32_t KSU_IOCTL_SET_FEATURE = _IOW(K, 14, uint64_t);
constexpr uint32_t KSU_IOCTL_GET_WRAPPER_FD = _IOW(K, 15, uint64_t);
constexpr uint32_t KSU_IOCTL_MANAGE_MARK = _IOWR(K, 16, uint64_t);
constexpr uint32_t KSU_IOCTL_NUKE_EXT4_SYSFS = _IOW(K, 17, uint64_t);
constexpr uint32_t KSU_IOCTL_ADD_TRY_UMOUNT = _IOW(K, 18, uint64_t);
constexpr uint32_t KSU_IOCTL_LIST_TRY_UMOUNT = _IOWR(K, 200, uint64_t);

// Structures for ioctl - use natural C alignment (matching kernel and Rust repr(C))
// Do NOT use #pragma pack(1) as it would misalign structures with the kernel!

struct GetInfoCmd {
    uint32_t version;
    uint32_t flags;
    uint32_t features;  // max feature ID supported
};

struct ReportEventCmd {
    uint32_t event;
};

struct SetSepolicyCmd {
    uint64_t cmd;
    uint64_t arg;
};

struct CheckSafemodeCmd {
    uint8_t in_safe_mode;
};

struct GetFeatureCmd {
    uint32_t feature_id;
    uint64_t value;
    uint8_t supported;
};

struct SetFeatureCmd {
    uint32_t feature_id;
    uint64_t value;
};

struct GetWrapperFdCmd {
    int32_t fd;
    uint32_t flags;
};

struct ManageMarkCmd {
    uint32_t operation;
    int32_t pid;
    uint32_t result;
};

struct NukeExt4SysfsCmd {
    uint64_t arg;
};

struct AddTryUmountCmd {
    uint64_t arg;
    uint32_t flags;
    uint8_t mode;
};

struct ListTryUmountCmd {
    uint64_t arg;
    uint32_t buf_size;
};

// API functions
int ksuctl(int request, void* arg);

int32_t get_version();
uint32_t get_flags();

int grant_root();
void report_post_fs_data();
void report_boot_complete();
void report_module_mounted();
bool check_kernel_safemode();

int set_sepolicy(const SetSepolicyCmd& cmd);

// Feature management
// Returns: pair<value, supported>
std::pair<uint64_t, bool> get_feature(uint32_t feature_id);
int set_feature(uint32_t feature_id, uint64_t value);

int get_wrapped_fd(int fd);

// Mark management
uint32_t mark_get(int32_t pid);
int mark_set(int32_t pid);
int mark_unset(int32_t pid);
int mark_refresh();

int nuke_ext4_sysfs(const std::string& mnt);

// Umount list management
int umount_list_wipe();
int umount_list_add(const std::string& path, uint32_t flags);
int umount_list_del(const std::string& path);
std::optional<std::string> umount_list_list();

}  // namespace ksud
