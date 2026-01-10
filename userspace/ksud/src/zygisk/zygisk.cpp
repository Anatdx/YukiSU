/**
 * YukiSU Zygisk Support Implementation
 *
 * Uses kernel IOCTL to detect zygote and coordinate injection.
 */

#include "zygisk.hpp"
#include "../core/ksucalls.hpp"
#include "../defs.hpp"
#include "../hymo/hymo_utils.hpp"
#include "../log.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static inline pid_t gettid() {
    return syscall(__NR_gettid);
}

namespace ksud {
namespace zygisk {

// IOCTL definitions (must match kernel/supercalls.h)
#define KSU_IOCTL_ZYGISK_WAIT_ZYGOTE _IOC(_IOC_READ, 'K', 120, 0)
#define KSU_IOCTL_ZYGISK_RESUME_ZYGOTE _IOC(_IOC_WRITE, 'K', 121, 0)
#define KSU_IOCTL_ZYGISK_ENABLE _IOC(_IOC_WRITE, 'K', 122, 0)

struct ksu_zygisk_wait_cmd {
    int32_t pid;
    uint8_t is_64bit;
    uint32_t timeout_ms;
} __attribute__((packed));

struct ksu_zygisk_resume_cmd {
    int32_t pid;
} __attribute__((packed));

struct ksu_zygisk_enable_cmd {
    uint8_t enable;
} __attribute__((packed));

// Tracer paths
static constexpr const char* TRACER_PATH_64 = "/data/adb/yukizygisk/bin/zygisk-ptrace64";
static constexpr const char* TRACER_PATH_32 = "/data/adb/yukizygisk/bin/zygisk-ptrace32";

// State
static std::atomic<bool> g_enabled{false};
static std::atomic<bool> g_running{false};
static std::thread g_monitor_thread;

// Enable zygisk in kernel
static bool kernel_enable_zygisk(int ksu_fd, bool enable) {
    ksu_zygisk_enable_cmd cmd = {static_cast<uint8_t>(enable ? 1 : 0)};
    int ret = ioctl(ksu_fd, KSU_IOCTL_ZYGISK_ENABLE, &cmd);
    if (ret < 0) {
        LOGE("IOCTL ZYGISK_ENABLE failed: %s", strerror(errno));
        return false;
    }
    LOGI("Zygisk %s in kernel", enable ? "enabled" : "disabled");
    return true;
}

// Wait for zygote
static bool kernel_wait_zygote(int ksu_fd, int* pid, bool* is_64bit, uint32_t timeout_ms) {
    ksu_zygisk_wait_cmd cmd;
    cmd.pid = 0;
    cmd.is_64bit = 0;
    cmd.timeout_ms = timeout_ms;

    int ret = ioctl(ksu_fd, KSU_IOCTL_ZYGISK_WAIT_ZYGOTE, &cmd);
    if (ret < 0) {
        if (errno == ETIMEDOUT) {
            return false;
        }
        LOGE("IOCTL ZYGISK_WAIT_ZYGOTE failed: %s", strerror(errno));
        return false;
    }

    *pid = cmd.pid;
    *is_64bit = cmd.is_64bit != 0;
    return true;
}

// Resume zygote
static bool kernel_resume_zygote(int ksu_fd, int pid) {
    ksu_zygisk_resume_cmd cmd;
    cmd.pid = pid;

    int ret = ioctl(ksu_fd, KSU_IOCTL_ZYGISK_RESUME_ZYGOTE, &cmd);
    if (ret < 0) {
        LOGE("IOCTL ZYGISK_RESUME_ZYGOTE failed: %s", strerror(errno));
        return false;
    }
    return true;
}

// Spawn tracer to inject
static void spawn_tracer(int target_pid, bool is_64bit) {
    const char* tracer = is_64bit ? TRACER_PATH_64 : TRACER_PATH_32;

    LOGI("spawn_tracer called: target_pid=%d is_64bit=%d tracer=%s", target_pid, is_64bit, tracer);

    if (access(tracer, X_OK) != 0) {
        LOGE("Tracer not accessible: %s (errno=%d: %s)", tracer, errno, strerror(errno));
        return;
    }

    LOGI("Spawning tracer for zygote pid=%d (%s)", target_pid, is_64bit ? "64-bit" : "32-bit");

    pid_t pid = fork();
    if (pid == 0) {
        char pid_str[16];
        snprintf(pid_str, sizeof(pid_str), "%d", target_pid);
        LOGI("Child: execl(%s, zygisk-ptrace, trace, %s)", tracer, pid_str);
        execl(tracer, "zygisk-ptrace", "trace", pid_str, nullptr);
        LOGE("execl tracer failed: %s", strerror(errno));
        _exit(1);
    } else if (pid > 0) {
        LOGI("Tracer forked with pid=%d, waiting...", pid);
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            LOGI("Tracer completed successfully (exit code 0)");
        } else if (WIFEXITED(status)) {
            LOGE("Tracer exited with code %d", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            LOGE("Tracer killed by signal %d", WTERMSIG(status));
        } else {
            LOGE("Tracer failed with status %d", status);
        }
    } else {
        LOGE("fork failed: %s", strerror(errno));
    }
}

// Check if zygote is already running and kill it to allow re-injection
static void kill_existing_zygote() {
    // Read /proc to find zygote processes
    DIR* proc = opendir("/proc");
    if (!proc) {
        return;
    }

    std::vector<pid_t> zygote_pids;
    struct dirent* entry;
    while ((entry = readdir(proc)) != nullptr) {
        // Only process numeric directories (PIDs)
        if (entry->d_type != DT_DIR)
            continue;
        char* endptr;
        pid_t pid = strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid <= 0)
            continue;

        // Read cmdline
        char cmdline_path[64];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", pid);
        int fd = open(cmdline_path, O_RDONLY);
        if (fd < 0)
            continue;

        char cmdline[256] = {0};
        read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);

        // Check if it's zygote
        if (strstr(cmdline, "zygote") != nullptr) {
            zygote_pids.push_back(pid);
        }
    }
    closedir(proc);

    // Kill all zygote processes
    for (pid_t pid : zygote_pids) {
        LOGI("Killing existing zygote pid=%d for re-injection", pid);
        kill(pid, SIGKILL);
    }

    if (!zygote_pids.empty()) {
        // Wait a bit for system to restart zygote
        usleep(100000);  // 100ms
    }
}

// Monitor thread function
static void monitor_thread_func() {
    LOGI("Zygisk monitor thread started (tid=%d)", gettid());

    // Get KSU fd (ksud already has it via hymo_utils)
    int ksu_fd = hymo::grab_ksu_fd();
    if (ksu_fd < 0) {
        LOGE("Cannot get KSU fd (fd=%d), zygisk disabled", ksu_fd);
        return;
    }
    LOGI("Got KSU fd=%d", ksu_fd);

    // Enable zygisk in kernel FIRST
    if (!kernel_enable_zygisk(ksu_fd, true)) {
        LOGE("Failed to enable zygisk in kernel");
        return;
    }

    // Check if zygote is already running - if so, kill it
    // System will restart zygote and we'll catch it this time
    kill_existing_zygote();

    g_running = true;
    LOGI("Monitor thread entering main loop");

    while (g_running && g_enabled) {
        int zygote_pid;
        bool is_64bit;

        LOGI("Calling kernel_wait_zygote (timeout=5000ms)...");
        // Wait for kernel to detect and pause zygote
        // Use 5 second timeout so we can check g_running periodically
        if (!kernel_wait_zygote(ksu_fd, &zygote_pid, &is_64bit, 5000)) {
            LOGI("kernel_wait_zygote returned false (timeout or error)");
            continue;
        }

        LOGI("Kernel detected zygote: pid=%d is_64bit=%d", zygote_pid, is_64bit);

        // Inject
        spawn_tracer(zygote_pid, is_64bit);

        // Resume zygote
        LOGI("Resuming zygote pid=%d", zygote_pid);
        kernel_resume_zygote(ksu_fd, zygote_pid);
    }

    // Disable zygisk in kernel
    kernel_enable_zygisk(ksu_fd, false);

    LOGI("Zygisk monitor thread stopped");
}

void start_zygisk_monitor() {
    if (g_monitor_thread.joinable()) {
        LOGW("Zygisk monitor already running");
        return;
    }

    // Check if zygisk files exist
    if (access(TRACER_PATH_64, X_OK) != 0 && access(TRACER_PATH_32, X_OK) != 0) {
        LOGI("Zygisk tracer not found, zygisk support disabled");
        return;
    }

    g_enabled = true;
    g_monitor_thread = std::thread(monitor_thread_func);
    LOGI("Zygisk monitor started");
}

void stop_zygisk_monitor() {
    g_enabled = false;
    g_running = false;

    if (g_monitor_thread.joinable()) {
        g_monitor_thread.join();
    }

    LOGI("Zygisk monitor stopped");
}

bool is_enabled() {
    return g_enabled;
}

void set_enabled(bool enable) {
    g_enabled = enable;
}

}  // namespace zygisk
}  // namespace ksud
