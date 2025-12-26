#include "kpm.hpp"
#include "log.hpp"

#include <cstdio>
#include <sys/syscall.h>
#include <unistd.h>

namespace ksud {

// KPM syscall number (arch-specific)
#ifdef __aarch64__
constexpr long KPM_SYSCALL_NUM = 458;
#else
constexpr long KPM_SYSCALL_NUM = -1;
#endif

enum KpmCmd {
    KPM_CMD_LOAD = 0,
    KPM_CMD_UNLOAD = 1,
    KPM_CMD_NUM = 2,
    KPM_CMD_LIST = 3,
    KPM_CMD_INFO = 4,
    KPM_CMD_CONTROL = 5,
    KPM_CMD_VERSION = 6,
};

static long kpm_syscall(int cmd, void* arg1 = nullptr, void* arg2 = nullptr) {
#ifdef __aarch64__
    return syscall(KPM_SYSCALL_NUM, cmd, arg1, arg2);
#else
    (void)cmd; (void)arg1; (void)arg2;
    return -1;
#endif
}

int kpm_load_module(const std::string& path, const std::optional<std::string>& args) {
#ifdef __aarch64__
    const char* args_ptr = args ? args->c_str() : nullptr;
    long ret = kpm_syscall(KPM_CMD_LOAD, 
                          const_cast<char*>(path.c_str()),
                          const_cast<char*>(args_ptr));
    if (ret < 0) {
        printf("Failed to load KPM module: %ld\n", ret);
        return 1;
    }
    printf("Loaded KPM module from %s\n", path.c_str());
    return 0;
#else
    printf("KPM is only supported on aarch64\n");
    return 1;
#endif
}

int kpm_unload_module(const std::string& name) {
#ifdef __aarch64__
    long ret = kpm_syscall(KPM_CMD_UNLOAD, const_cast<char*>(name.c_str()));
    if (ret < 0) {
        printf("Failed to unload KPM module: %ld\n", ret);
        return 1;
    }
    printf("Unloaded KPM module: %s\n", name.c_str());
    return 0;
#else
    printf("KPM is only supported on aarch64\n");
    return 1;
#endif
}

int kpm_num() {
#ifdef __aarch64__
    long ret = kpm_syscall(KPM_CMD_NUM);
    if (ret < 0) {
        printf("Failed to get KPM module count: %ld\n", ret);
        return 1;
    }
    printf("Loaded KPM modules: %ld\n", ret);
    return 0;
#else
    printf("KPM is only supported on aarch64\n");
    return 1;
#endif
}

int kpm_list() {
#ifdef __aarch64__
    char buf[4096] = {0};
    long ret = kpm_syscall(KPM_CMD_LIST, buf);
    if (ret < 0) {
        printf("Failed to list KPM modules: %ld\n", ret);
        return 1;
    }
    printf("%s", buf);
    return 0;
#else
    printf("KPM is only supported on aarch64\n");
    return 1;
#endif
}

int kpm_info(const std::string& name) {
#ifdef __aarch64__
    char buf[1024] = {0};
    long ret = kpm_syscall(KPM_CMD_INFO, const_cast<char*>(name.c_str()), buf);
    if (ret < 0) {
        printf("Failed to get KPM module info: %ld\n", ret);
        return 1;
    }
    printf("%s\n", buf);
    return 0;
#else
    printf("KPM is only supported on aarch64\n");
    return 1;
#endif
}

int kpm_control(const std::string& name, const std::string& args) {
#ifdef __aarch64__
    char buf[1024] = {0};
    // Pass both name and args
    std::string combined = name + "\0" + args;
    long ret = kpm_syscall(KPM_CMD_CONTROL, const_cast<char*>(combined.c_str()), buf);
    if (ret < 0) {
        printf("Failed to send control command: %ld\n", ret);
        return 1;
    }
    printf("%s\n", buf);
    return 0;
#else
    printf("KPM is only supported on aarch64\n");
    return 1;
#endif
}

int kpm_version() {
#ifdef __aarch64__
    char buf[64] = {0};
    long ret = kpm_syscall(KPM_CMD_VERSION, buf);
    if (ret < 0) {
        printf("Failed to get KPM version: %ld\n", ret);
        return 1;
    }
    printf("KPM Loader version: %s\n", buf);
    return 0;
#else
    printf("KPM is only supported on aarch64\n");
    return 1;
#endif
}

} // namespace ksud
