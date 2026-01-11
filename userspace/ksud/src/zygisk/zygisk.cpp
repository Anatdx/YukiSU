/**
 * YukiSU Zygisk Support Implementation
 *
 * Uses kernel IOCTL to detect zygote and coordinate injection.
 * Integrates YukiZygisk ptracer for built-in injection.
 */

#include "zygisk.hpp"
#include "../core/ksucalls.hpp"
#include "../defs.hpp"
#include "../hymo/hymo_utils.hpp"
#include "../log.hpp"
#include "ptracer/injector.hpp"

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
#include <sys/ptrace.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX 92
#endif // #ifndef PROP_VALUE_MAX

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
};

struct ksu_zygisk_resume_cmd {
    int32_t pid;
};

struct ksu_zygisk_enable_cmd {
    uint8_t enable;
};

// Payload paths for zygisk injection
static constexpr const char* PAYLOAD_PATH_64 = "/data/adb/yukizygisk/lib64/libzygisk.so";
static constexpr const char* PAYLOAD_PATH_32 = "/data/adb/yukizygisk/lib/libzygisk.so";

// State
static std::atomic<bool> g_injection_active{false};
static std::thread g_injection_thread;

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
    ksu_zygisk_wait_cmd cmd = {};  // Zero-initialize including padding
    cmd.timeout_ms = timeout_ms;

    int ret = ioctl(ksu_fd, KSU_IOCTL_ZYGISK_WAIT_ZYGOTE, &cmd);

    // DEBUG: IOCTL result
    int fd_ioctl =
        open("/data/local/tmp/zygisk_ioctl_wait_result", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd_ioctl >= 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "ret=%d errno=%d(%s) pid=%d is_64bit=%d\n", ret, errno,
                 strerror(errno), cmd.pid, cmd.is_64bit);
        write(fd_ioctl, buf, strlen(buf));
        close(fd_ioctl);
    }

    if (ret < 0) {
        // EINTR: interrupted by signal, caller should retry
        // ETIMEDOUT: timeout (expected, not an error)
        if (errno == ETIMEDOUT || errno == EINTR) {
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

// Inject zygote using built-in ptrace injector
static void inject_zygote(int target_pid, bool is_64bit) {
    const char* payload = is_64bit ? PAYLOAD_PATH_64 : PAYLOAD_PATH_32;

    LOGI("inject_zygote: target_pid=%d is_64bit=%d payload=%s", target_pid, is_64bit, payload);

    // Check payload exists BEFORE injecting
    if (access(payload, R_OK) != 0) {
        LOGE("Payload not accessible: %s (errno=%d: %s) - ABORT injection", payload, errno,
             strerror(errno));
        return;
    }

    // Use YukiZygisk's injector
    LOGI("Calling YukiZygisk injector for pid=%d...", target_pid);
    bool success = yuki::ptracer::injectOnMain(target_pid, payload);

    if (success) {
        LOGI("YukiZygisk injection succeeded for pid=%d", target_pid);
    } else {
        LOGE("YukiZygisk injection failed for pid=%d", target_pid);
    }
}

// Check if process is in stopped state
static bool is_process_stopped(pid_t pid) {
    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);

    int fd = open(stat_path, O_RDONLY);
    if (fd < 0) {
        LOGE("Cannot open %s: %s", stat_path, strerror(errno));
        return false;
    }

    char stat[512] = {0};
    ssize_t n = read(fd, stat, sizeof(stat) - 1);
    close(fd);

    if (n <= 0) {
        LOGE("Cannot read %s", stat_path);
        return false;
    }

    // Format: pid (comm) state ...
    // state is the first character after the second ')'
    char* p = strrchr(stat, ')');
    if (!p || p[1] != ' ') {
        LOGE("Invalid stat format");
        return false;
    }

    char state = p[2];
    // T = stopped (on a signal)
    // t = tracing stop
    bool is_stopped = (state == 'T' || state == 't');
    LOGI("Process %d state: %c (stopped=%d)", pid, state, is_stopped);
    return is_stopped;
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

// Injection thread function - runs ONCE to inject both zygotes then exits
static void injection_thread_func() {
    LOGI("Zygisk injection thread started (tid=%d)", gettid());

    // DEBUG: Thread function entered
    int fd = open("/data/local/tmp/zygisk_thread_func_entered", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        close(fd);
    }

    // Get KSU fd
    int ksu_fd = hymo::grab_ksu_fd();
    if (ksu_fd < 0) {
        LOGE("Cannot get KSU fd (fd=%d), injection aborted", ksu_fd);

        // DEBUG: KSU fd failed
        int fd2 = open("/data/local/tmp/zygisk_ksu_fd_failed", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd2 >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "fd=%d\n", ksu_fd);
            write(fd2, buf, strlen(buf));
            close(fd2);
        }
        return;
    }
    LOGI("Got KSU fd=%d", ksu_fd);

    // DEBUG: Got KSU fd
    int fd3 = open("/data/local/tmp/zygisk_got_ksu_fd", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd3 >= 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "fd=%d\n", ksu_fd);
        write(fd3, buf, strlen(buf));
        close(fd3);
    }

    // Enable zygisk in kernel NOW - before post-fs-data ends
    LOGI("Calling kernel_enable_zygisk(fd=%d, enable=true)...", ksu_fd);
    if (!kernel_enable_zygisk(ksu_fd, true)) {
        LOGE("Failed to enable zygisk in kernel - IOCTL returned error");
        LOGE("Possible causes: 1) Kernel module not loaded 2) IOCTL not implemented");
        LOGE("Injection thread aborting - zygote will start normally");

        // DEBUG: kernel_enable_zygisk failed
        int fd4 =
            open("/data/local/tmp/zygisk_kernel_enable_failed", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd4 >= 0) {
            close(fd4);
        }
        return;
    }
    LOGI("Zygisk successfully enabled in kernel - waiting for zygotes...");

    // DEBUG: Kernel enabled successfully
    int fd5 = open("/data/local/tmp/zygisk_kernel_enabled", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd5 >= 0) {
        close(fd5);
    }

    // Poll kernel for zygote detection (100ms interval, 15s total timeout)
    int zygote_pid = -1;
    bool is_64bit = false;
    const int POLL_INTERVAL_MS = 100;
    const int MAX_POLL_ATTEMPTS = 150;  // 15 seconds total

    LOGI("Polling kernel for zygote detection (100ms interval, 15s timeout)...");

    bool caught = false;
    for (int attempt = 0; attempt < MAX_POLL_ATTEMPTS; attempt++) {
        // Ask kernel: "Did you catch a zygote yet?"
        if (kernel_wait_zygote(ksu_fd, &zygote_pid, &is_64bit, POLL_INTERVAL_MS)) {
            caught = true;
            LOGI("Kernel caught zygote after %d attempts (%.1fs): pid=%d is_64bit=%d", attempt + 1,
                 (attempt + 1) * POLL_INTERVAL_MS / 1000.0, zygote_pid, is_64bit);
            break;
        }
        // No zygote yet, continue polling...
    }

    if (!caught) {
        LOGE("Kernel did not catch zygote within timeout (15s) - injection aborted");
        return;
    }

    // Verify process is stopped by kernel
    if (!is_process_stopped(zygote_pid)) {
        LOGE("Zygote pid=%d not stopped by kernel - cannot inject safely", zygote_pid);
        // Resume to avoid hanging system
        kernel_resume_zygote(ksu_fd, zygote_pid);
        return;
    }

    LOGI("Zygote pid=%d confirmed stopped, injecting...", zygote_pid);

    // Inject libzygisk.so
    inject_zygote(zygote_pid, is_64bit);

    // CRITICAL: Always resume zygote, even if injection failed
    LOGI("Resuming zygote pid=%d", zygote_pid);
    if (!kernel_resume_zygote(ksu_fd, zygote_pid)) {
        LOGE("FATAL: kernel_resume_zygote failed for pid=%d", zygote_pid);
    }

    // Keep zygisk enabled - kernel will continue monitoring for other zygotes
    // User can disable via CLI command if needed
    LOGI("Zygisk injection complete (zygisk remains enabled), thread exiting");
}

// Enable zygisk and start injection in background (called from Phase 0)
void enable_and_inject_async() {
    // DEBUG: Prove function is called
    int fd =
        open("/data/local/tmp/zygisk_enable_and_inject_called", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        close(fd);
    }

    LOGI("=== enable_and_inject_async called ===");

    if (g_injection_thread.joinable()) {
        LOGW("Zygisk injection already running");
        return;
    }

    // Check for payload files
    bool has_payload = (access(PAYLOAD_PATH_64, R_OK) == 0 || access(PAYLOAD_PATH_32, R_OK) == 0);

    // DEBUG: File check result
    int fd2 = open("/data/local/tmp/zygisk_file_check", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd2 >= 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "payload=%d\n", has_payload);
        write(fd2, buf, strlen(buf));
        close(fd2);
    }

    LOGI("Zygisk file check: payload=%d", has_payload);

    if (!has_payload) {
        LOGE("Zygisk payload files not found");
        LOGE("Checked paths: %s %s", PAYLOAD_PATH_64, PAYLOAD_PATH_32);

        // DEBUG: Mark early return
        int fd3 = open("/data/local/tmp/zygisk_no_files", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd3 >= 0) {
            close(fd3);
        }
        return;
    }

    LOGI("Payload .so files available for built-in injection");

    // Start injection thread (async, non-blocking)
    // CRITICAL: Thread must be detached IMMEDIATELY to avoid blocking daemon
    g_injection_active = true;
    try {
        g_injection_thread = std::thread(injection_thread_func);
        g_injection_thread.detach();  // Detach before ANY potential blocking
        LOGI("Zygisk injection thread started (async, detached)");

        // DEBUG: Thread created successfully
        int fd4 = open("/data/local/tmp/zygisk_thread_created", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd4 >= 0) {
            close(fd4);
        }
    } catch (const std::exception& e) {
        LOGE("Failed to start injection thread: %s", e.what());
        g_injection_active = false;

        // DEBUG: Thread creation failed
        int fd5 = open("/data/local/tmp/zygisk_thread_failed", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd5 >= 0) {
            write(fd5, e.what(), strlen(e.what()));
            close(fd5);
        }
    }
}

// Legacy functions for CLI compatibility
bool is_enabled() {
    return access("/data/adb/.yukizenable", F_OK) == 0;
}

void set_enabled(bool enable) {
    // Managed by CLI commands, not runtime
}

}  // namespace zygisk
}  // namespace ksud
