#include "lkm.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include "../../assets.hpp"
#include "../defs.hpp"
#include "../mount/hymofs.hpp"
#include "../utils.hpp"

namespace fs = std::filesystem;
namespace hymo {

static constexpr int HYMO_SYSCALL_NR = 142;
static std::string g_lkm_last_error;

static void set_lkm_last_error(const std::string& msg) {
    g_lkm_last_error = msg;
}

// finit_module and init_module syscall numbers
#if defined(__aarch64__)
#define SYS_init_module_num 105
#define SYS_finit_module_num 379
#define SYS_delete_module_num 106
#elif defined(__x86_64__) || defined(__i386__)
#define SYS_init_module_num 175
#define SYS_finit_module_num 313
#define SYS_delete_module_num 176
#elif defined(__arm__)
#define SYS_init_module_num 128
#define SYS_finit_module_num 379
#define SYS_delete_module_num 129
#else
#define SYS_init_module_num 105
#define SYS_finit_module_num 379
#define SYS_delete_module_num 106
#endif  // #if defined(__aarch64__)

static bool load_module_via_init(const char* ko_path, const char* params) {
    int fd = open(ko_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR(std::string("lkm: open ") + ko_path + " failed: " + strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        LOG_ERROR(std::string("lkm: fstat failed: ") + strerror(errno));
        close(fd);
        return false;
    }

    size_t size = st.st_size;
    void* image = malloc(size);
    if (!image) {
        LOG_ERROR("lkm: malloc failed");
        close(fd);
        return false;
    }

    size_t bytes_read = 0;
    while (bytes_read < size) {
        ssize_t r = read(fd, (char*)image + bytes_read, size - bytes_read);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            LOG_ERROR(std::string("lkm: read failed: ") + strerror(errno));
            free(image);
            close(fd);
            return false;
        }
        if (r == 0)
            break;  // EOF
        bytes_read += r;
    }
    close(fd);

    int ret = syscall(SYS_init_module_num, image, size, params);
    free(image);

    if (ret != 0) {
        if (errno == EEXIST) {
            LOG_VERBOSE("lkm: init_module skipped (module already loaded)");
            return true;
        }
        LOG_ERROR(std::string("lkm: init_module ") + ko_path + " failed: " + strerror(errno));
        return false;
    }
    return true;
}

static bool load_module_via_finit(const char* ko_path, const char* params) {
    const int fd = open(ko_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR(std::string("lkm: open ") + ko_path + " failed: " + strerror(errno));
        return false;
    }
    const int ret = syscall(SYS_finit_module_num, fd, params, 0);
    close(fd);
    if (ret != 0) {
        if (errno == ENOSYS) {
            LOG_WARN("finit_module not implemented, falling back to init_module");
            return load_module_via_init(ko_path, params);
        }
        if (errno == EEXIST) {
            LOG_VERBOSE("lkm: finit_module skipped (module already loaded)");
            return true;
        }
        LOG_ERROR(std::string("lkm: finit_module ") + ko_path + " failed: " + strerror(errno));
        return false;
    }
    return true;
}

static bool unload_module_via_syscall(const char* modname) {
    // Use blocking unload here. Non-blocking delete_module often returns EAGAIN
    // while references are draining, whereas user-facing rmmod typically waits.
    const int ret = syscall(SYS_delete_module_num, modname, 0);
    if (ret != 0) {
        set_lkm_last_error(std::string("delete_module ") + modname + " failed: " + strerror(errno));
        LOG_ERROR(std::string("lkm: ") + g_lkm_last_error);
        return false;
    }
    return true;
}

static bool unload_module_via_rmmod(const char* modname) {
    const std::string cmd = std::string("/system/bin/rmmod ") + modname + " >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc == -1) {
        set_lkm_last_error(std::string("failed to exec rmmod for ") + modname);
        LOG_ERROR(std::string("lkm: ") + g_lkm_last_error);
        return false;
    }
    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) {
        return true;
    }
    set_lkm_last_error(
        std::string("rmmod ") + modname + " failed, wait_status=" + std::to_string(rc) +
        ", exit_code=" + (WIFEXITED(rc) ? std::to_string(WEXITSTATUS(rc)) : std::string("signal")));
    LOG_ERROR(std::string("lkm: ") + g_lkm_last_error);
    return false;
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

#include <sys/utsname.h>

// Read real kernel release from sysfs. Not spoofed by HymoFS uname hiding (uname(2) is).
// Use this for KMI matching so LKM installation picks the correct module for the real kernel.
static std::string read_kernel_release_from_sysfs() {
    return read_file_first_line("/proc/sys/kernel/osrelease");
}

static std::string get_current_kmi() {
    std::string full_version = read_kernel_release_from_sysfs();
    if (full_version.empty()) {
        struct utsname uts{};
        if (uname(&uts) != 0) {
            LOG_ERROR("Failed to get uname");
            return "";
        }
        full_version = uts.release;
    }

    const size_t dot1 = full_version.find('.');
    if (dot1 == std::string::npos)
        return "";
    size_t dot2 = full_version.find('.', dot1 + 1);
    if (dot2 == std::string::npos)
        dot2 = full_version.length();

    std::string major_minor = full_version.substr(0, dot2);

    const size_t android_pos = full_version.find("-android");
    if (android_pos != std::string::npos) {
        const size_t ver_start = android_pos + 8;
        size_t ver_end = full_version.find('-', ver_start);
        if (ver_end == std::string::npos)
            ver_end = full_version.length();

        const std::string android_ver = full_version.substr(ver_start, ver_end - ver_start);
        return "android" + android_ver + "-" + major_minor;
    }

    return "";
}

// Arch suffix for embedded hymofs .ko
#if defined(__aarch64__)
#define HYMO_ARCH_SUFFIX "_arm64"
#elif defined(__arm__)
#define HYMO_ARCH_SUFFIX "_armv7"
#elif defined(__x86_64__)
#define HYMO_ARCH_SUFFIX "_x86_64"
#else
#define HYMO_ARCH_SUFFIX "_arm64"
#endif  // #if defined(__aarch64__)

bool lkm_is_loaded() {
    return HymoFS::is_available();
}

std::string lkm_get_last_error() {
    return g_lkm_last_error;
}

std::string lkm_get_kmi_override() {
    return read_file_first_line(LKM_KMI_OVERRIDE_FILE);
}

bool lkm_set_kmi_override(const std::string& kmi) {
    if (!ensure_base_dir())
        return false;
    return write_file(LKM_KMI_OVERRIDE_FILE, kmi);
}

bool lkm_clear_kmi_override() {
    if (unlink(LKM_KMI_OVERRIDE_FILE) == 0)
        return true;
    return errno == ENOENT;
}

void lkm_autoload_post_fs_data() {
    if (lkm_get_autoload() && !lkm_is_loaded()) {
        lkm_load();
    }
}

bool lkm_load() {
    set_lkm_last_error("");
    if (lkm_is_loaded()) {
        return true;
    }

    std::string ko_path;
    std::string kmi = lkm_get_kmi_override();
    if (kmi.empty()) {
        kmi = get_current_kmi();
    }

    if (!kmi.empty() && ensure_base_dir()) {
        const std::string asset_name = kmi + HYMO_ARCH_SUFFIX "_hymofs_lkm.ko";

        char tmp_path[256];
        snprintf(tmp_path, sizeof(tmp_path), "%s/.lkm_XXXXXX", HYMO_DATA_DIR);
        int tmp_fd = mkstemp(tmp_path);
        if (tmp_fd >= 0) {
            close(tmp_fd);
            if (ksud::copy_asset_to_file(asset_name, tmp_path)) {
                ko_path = tmp_path;
            } else {
                unlink(tmp_path);
            }
        }
    }

    // Fallback to legacy path if not embedded
    if (ko_path.empty() && fs::exists(LKM_KO)) {
        ko_path = LKM_KO;
    }

    if (ko_path.empty()) {
        set_lkm_last_error("no matching module found for " + kmi);
        LOG_ERROR("HymoFS LKM: " + g_lkm_last_error);
        return false;
    }

    char params[64];
    snprintf(params, sizeof(params), "hymo_syscall_nr=%d", HYMO_SYSCALL_NR);

    bool ok = load_module_via_finit(ko_path.c_str(), params);

    // Cleanup temp file if we extracted it
    if (ko_path != LKM_KO) {
        unlink(ko_path.c_str());
    }

    return ok;
}

bool lkm_unload() {
    set_lkm_last_error("");
    // Idempotent behavior: already unloaded should not be treated as an error.
    if (!lkm_is_loaded()) {
        return true;
    }
    if (HymoFS::is_available()) {
        // Disable first to reduce active hook traffic during unload window.
        HymoFS::set_enabled(false);
        if (!HymoFS::clear_rules()) {
            set_lkm_last_error("failed to clear HymoFS rules before unload");
        }
        // Release cached HymoFS anon-fd in this process. Otherwise the module may
        // stay busy until ksud exits, causing immediate unload attempts to fail.
        HymoFS::release_connection();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    // delete_module may return EAGAIN/EBUSY while hooks are still being released.
    for (int i = 0; i < 5; ++i) {
        if (unload_module_via_syscall("hymofs_lkm")) {
            return true;
        }
        if (errno != EAGAIN && errno != EBUSY) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    LOG_WARN("lkm: delete_module failed, fallback to rmmod");
    if (unload_module_via_rmmod("hymofs_lkm")) {
        return true;
    }
    if (g_lkm_last_error.find("delete_module") != std::string::npos ||
        g_lkm_last_error.find("rmmod") != std::string::npos) {
        g_lkm_last_error += " (module may still be busy; stop related mounts/processes or reboot)";
    }
    return false;
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

}  // namespace hymo
