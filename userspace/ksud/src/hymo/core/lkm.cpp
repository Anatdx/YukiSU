#include "lkm.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include "../../assets.hpp"
#include "../../boot/boot_patch.hpp"
#include "../../log.hpp"
#include "../defs.hpp"
#include "../mount/hymofs.hpp"

namespace fs = std::filesystem;
namespace hymo {

static constexpr int HYMO_SYSCALL_NR = 142;

// finit_module: Linux aarch64=379, x86_64=313
#if defined(__aarch64__)
#define SYS_finit_module_num 379
#define SYS_delete_module_num 106
#elif defined(__x86_64__) || defined(__i386__)
#define SYS_finit_module_num 313
#define SYS_delete_module_num 176
#else
#define SYS_finit_module_num 379
#define SYS_delete_module_num 106
#endif  // #if defined(__aarch64__)

// Load kernel module via finit_module syscall (no shell)
static bool load_module_via_finit(const char* ko_path, const char* params) {
    const int fd = open(ko_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("lkm: open %s failed: %s", ko_path, strerror(errno));
        return false;
    }
    const int ret = syscall(SYS_finit_module_num, fd, params, 0);
    close(fd);
    if (ret != 0) {
        LOGE("lkm: finit_module %s failed: %s", ko_path, strerror(errno));
        return false;
    }
    return true;
}

// Unload kernel module via delete_module syscall (no shell)
static bool unload_module_via_syscall(const char* modname) {
    const int ret = syscall(SYS_delete_module_num, modname, O_NONBLOCK);
    if (ret != 0) {
        LOGE("lkm: delete_module %s failed: %s", modname, strerror(errno));
        return false;
    }
    return true;
}

static std::string read_file_first_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (std::getline(f, line)) {
        return line;
    }
    return "";
}

static bool write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f)
        return false;
    f << content;
    return f.good();
}

static bool ensure_base_dir() {
    try {
        fs::create_directories(BASE_DIR);
        return true;
    } catch (...) {
        return false;
    }
}

bool lkm_is_loaded() {
    return HymoFS::is_available();
}

// Arch suffix for embedded hymofs .ko (matches workflow artifact naming)
#if defined(__aarch64__)
#define HYMO_ARCH_SUFFIX "_arm64"
#elif defined(__arm__)
#define HYMO_ARCH_SUFFIX "_armv7"
#elif defined(__x86_64__)
#define HYMO_ARCH_SUFFIX "_x86_64"
#else
#define HYMO_ARCH_SUFFIX "_arm64"  // default
#endif                             // #if defined(__aarch64__)

bool lkm_load() {
    std::string ko_path;
    const std::string kmi = ksud::get_current_kmi();

    if (!kmi.empty() && ensure_base_dir()) {
        const std::string asset_name = kmi + HYMO_ARCH_SUFFIX "_hymofs_lkm.ko";
        // Extract to temp file, load, then unlink (extract-load-cleanup)
        char tmp_path[256];
        snprintf(tmp_path, sizeof(tmp_path), "%s/.lkm_XXXXXX", HYMO_DATA_DIR);
        int tmp_fd = mkstemp(tmp_path);
        if (tmp_fd >= 0) {
            close(tmp_fd);
            if (ksud::copy_asset_to_file(asset_name, tmp_path)) {
                ko_path = tmp_path;
            }
            if (!ko_path.empty()) {
                char params[64];
                snprintf(params, sizeof(params), "hymo_syscall_nr=%d", HYMO_SYSCALL_NR);
                const bool ok = load_module_via_finit(ko_path.c_str(), params);
                unlink(tmp_path);
                if (ok)
                    return true;
            } else {
                unlink(tmp_path);
            }
        }
    }

    if (fs::exists(LKM_KO)) {
        ko_path = LKM_KO;
    }

    if (ko_path.empty()) {
        return false;
    }

    char params[64];
    snprintf(params, sizeof(params), "hymo_syscall_nr=%d", HYMO_SYSCALL_NR);
    return load_module_via_finit(ko_path.c_str(), params);
}

bool lkm_unload() {
    if (HymoFS::is_available()) {
        HymoFS::clear_rules();
    }
    return unload_module_via_syscall("hymofs_lkm");
}

bool lkm_set_autoload(bool on) {
    if (!ensure_base_dir())
        return false;
    return write_file(LKM_AUTOLOAD_FILE, on ? "1" : "0");
}

bool lkm_get_autoload() {
    std::string v = read_file_first_line(LKM_AUTOLOAD_FILE);
    if (v.empty())
        return true;  // default on
    return (v == "1" || v == "on" || v == "true");
}

static void lkm_autoload_log_failure(const char* msg) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);
    char ts[64];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03ld", tm_buf.tm_year + 1900,
             tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             static_cast<long>(tv.tv_usec / 1000));
    std::ofstream f(LKM_AUTOLOAD_LOG_FILE, std::ios::app);
    if (f) {
        f << "[" << ts << "] " << msg << "\n";
    }
}

// Called from post-fs-data: extract embedded LKM, load, cleanup. No shell.
// Ksud knows its arch; embedded hymofs .ko matches each ksud build (arm64/armv7/x86_64).
void lkm_autoload_post_fs_data() {
    if (!lkm_get_autoload()) {
        LOGI("HymoFS LKM autoload disabled, skip");
        return;
    }
    if (lkm_is_loaded()) {
        LOGI("HymoFS LKM already loaded, skip");
        return;
    }

    const std::string kmi = ksud::get_current_kmi();
    if (kmi.empty()) {
        const char* msg = "HymoFS LKM: cannot detect KMI, skip";
        LOGW("%s", msg);
        lkm_autoload_log_failure(msg);
        return;
    }

    if (!ensure_base_dir()) {
        const char* msg = "HymoFS LKM: cannot create " HYMO_DATA_DIR;
        LOGW("%s", msg);
        lkm_autoload_log_failure(msg);
        return;
    }

    const std::string asset_name = kmi + HYMO_ARCH_SUFFIX "_hymofs_lkm.ko";
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.lkm_XXXXXX", HYMO_DATA_DIR);
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "HymoFS LKM: mkstemp failed: %s", strerror(errno));
        LOGW("%s", msg);
        lkm_autoload_log_failure(msg);
        return;
    }
    close(tmp_fd);

    if (!ksud::copy_asset_to_file(asset_name, tmp_path)) {
        unlink(tmp_path);
        // Fallback: try LKM_KO from Magisk module
        if (fs::exists(LKM_KO)) {
            char params[64];
            snprintf(params, sizeof(params), "hymo_syscall_nr=%d", HYMO_SYSCALL_NR);
            if (load_module_via_finit(LKM_KO, params)) {
                LOGI("HymoFS LKM loaded from " HYMO_MODULE_DIR);
                return;
            }
        }
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "HymoFS LKM: no matching module (asset %s, fallback " HYMO_MODULE_DIR " failed)",
                 asset_name.c_str());
        LOGW("%s", msg);
        lkm_autoload_log_failure(msg);
        return;
    }

    char params[64];
    snprintf(params, sizeof(params), "hymo_syscall_nr=%d", HYMO_SYSCALL_NR);
    if (load_module_via_finit(tmp_path, params)) {
        LOGI("HymoFS LKM loaded from embedded %s", asset_name.c_str());
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "HymoFS LKM: finit_module failed for %s", asset_name.c_str());
        lkm_autoload_log_failure(msg);
    }
    unlink(tmp_path);
}

}  // namespace hymo
