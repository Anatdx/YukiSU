/**
 * ksuinit - Init module
 * 
 * Handles the initialization sequence:
 * - Mount filesystems
 * - Setup logging
 * - Detect GKI KernelSU
 * - Load LKM
 * - Setup real init
 */

#include "init.hpp"
#include "loader.hpp"
#include "log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ksuinit {

namespace {

/**
 * RAII class for auto-unmounting filesystems
 */
class AutoUmount {
public:
    ~AutoUmount() {
        // Unmount in reverse order
        for (auto it = mountpoints_.rbegin(); it != mountpoints_.rend(); ++it) {
            if (umount2(it->c_str(), MNT_DETACH) != 0) {
                KLOGE("Cannot umount %s: %s", it->c_str(), strerror(errno));
            }
        }
    }

    void add(const std::string& mountpoint) {
        mountpoints_.push_back(mountpoint);
    }

private:
    std::vector<std::string> mountpoints_;
};

/**
 * Mount a filesystem
 */
bool mount_filesystem(const char* fstype, const char* mountpoint) {
    // Create mountpoint if it doesn't exist
    if (mkdir(mountpoint, 0755) != 0 && errno != EEXIST) {
        KLOGE("Cannot create mountpoint %s: %s", mountpoint, strerror(errno));
        return false;
    }

    // Mount the filesystem
    if (mount(fstype, mountpoint, fstype, 0, nullptr) != 0) {
        KLOGE("Cannot mount %s on %s: %s", fstype, mountpoint, strerror(errno));
        return false;
    }

    return true;
}

/**
 * Prepare mount points (/proc, /sys)
 */
AutoUmount prepare_mount() {
    AutoUmount auto_umount;

    // Mount procfs
    if (mount_filesystem("proc", "/proc")) {
        auto_umount.add("/proc");
    }

    // Mount sysfs
    if (mount_filesystem("sysfs", "/sys")) {
        auto_umount.add("/sys");
    }

    return auto_umount;
}

/**
 * Setup kernel logging via /dev/kmsg
 */
void setup_kmsg() {
    const char* device = "/dev/kmsg";

    // Check if /dev/kmsg exists
    if (access(device, F_OK) != 0) {
        // Try to create it
        if (mknod("/kmsg", S_IFCHR | 0666, makedev(1, 11)) == 0) {
            device = "/kmsg";
        }
    }

    // Initialize kernel log
    log_init(device);
}

/**
 * Disable kmsg rate limiting
 */
void unlimit_kmsg() {
    std::ofstream rate("/proc/sys/kernel/printk_devkmsg");
    if (rate.is_open()) {
        rate << "on\n";
    }
}

/**
 * Check if KernelSU is present using the new v2 method (ioctl)
 */
bool has_kernelsu_v2() {
    constexpr uint32_t KSU_INSTALL_MAGIC1 = 0xDEADBEEF;
    constexpr uint32_t KSU_INSTALL_MAGIC2 = 0xCAFEBABE;
    constexpr uint32_t KSU_IOCTL_GET_INFO = 0x80004b02; // _IOC(_IOC_READ, 'K', 2, 0)

    struct GetInfoCmd {
        uint32_t version;
        uint32_t flags;
    };

    // Try to get driver fd using reboot syscall with magic numbers
    int fd = -1;
    syscall(__NR_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd);

    uint32_t version = 0;
    if (fd >= 0) {
        // Try to get version info via ioctl
        GetInfoCmd cmd = {0, 0};
        if (ioctl(fd, KSU_IOCTL_GET_INFO, &cmd) == 0) {
            version = cmd.version;
        }
        close(fd);
    }

    KLOGI("KernelSU version (v2): %u", version);
    return version != 0;
}

/**
 * Check if KernelSU is present using the legacy method (prctl)
 */
bool has_kernelsu_legacy() {
    constexpr int CMD_GET_VERSION = 2;
    
    uint32_t version = 0;
    syscall(__NR_prctl, 0xDEADBEEF, CMD_GET_VERSION, &version, 0, 0);

    KLOGI("KernelSU version (legacy): %u", version);
    return version != 0;
}

} // anonymous namespace

bool has_kernelsu() {
    return has_kernelsu_v2() || has_kernelsu_legacy();
}

bool init() {
    // Setup kernel log first
    setup_kmsg();

    KLOGI("Hello, KernelSU!");

    // Mount /proc and /sys to access kernel interface
    // They will be auto-unmounted when this scope exits
    {
        auto auto_umount = prepare_mount();

        // Disable kmsg rate limiting (requires /proc)
        unlimit_kmsg();

        // Check if GKI KernelSU is present
        // LKM priority mode: always load LKM even if GKI exists
        // GKI will yield when LKM sends YIELD command
        if (has_kernelsu()) {
            KLOGI("KernelSU GKI detected, LKM will take over...");
        }

        // Load the KernelSU LKM module
        KLOGI("Loading kernelsu.ko..");
        if (!load_module("/kernelsu.ko")) {
            KLOGE("Cannot load kernelsu.ko");
        }
    }
    // /proc and /sys are unmounted here

    // Remove the current /init (which is us)
    if (unlink("/init") != 0) {
        KLOGE("Cannot unlink /init: %s", strerror(errno));
        return false;
    }

    // Determine the real init path
    const char* real_init = "/system/bin/init";
    if (access("/init.real", F_OK) == 0) {
        real_init = "init.real";
    }

    KLOGI("init is %s", real_init);

    // Create symlink to real init
    if (symlink(real_init, "/init") != 0) {
        KLOGE("Cannot symlink %s to /init: %s", real_init, strerror(errno));
        return false;
    }

    return true;
}

} // namespace ksuinit
