#include "lkm.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include "../../assets.hpp"
#include "../defs.hpp"
#include "../mount/hymofs.hpp"
#include "../utils.hpp"

namespace fs = std::filesystem;
namespace hymo {

static constexpr int HYMO_SYSCALL_NR = 142;

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
        LOG_ERROR(std::string("lkm: finit_module ") + ko_path + " failed: " + strerror(errno));
        return false;
    }
    return true;
}

static bool unload_module_via_syscall(const char* modname) {
    const int ret = syscall(SYS_delete_module_num, modname, O_NONBLOCK);
    if (ret != 0) {
        LOG_ERROR(std::string("lkm: delete_module ") + modname + " failed: " + strerror(errno));
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

#include <sys/utsname.h>

static std::string get_current_kmi() {
    struct utsname uts{};
    if (uname(&uts) != 0) {
        LOG_ERROR("Failed to get uname");
        return "";
    }

    const std::string full_version = uts.release;

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
        LOG_ERROR("HymoFS LKM: no matching module found for " + kmi);
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

}  // namespace hymo
