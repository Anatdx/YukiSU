// Murasaki Binder Service - Native Implementation
// 使用 libbinder_ndk 向 ServiceManager 注册服务
// Android-only: Uses libbinder_ndk API

#ifdef __ANDROID__

#include "murasaki_binder.hpp"
#include "../core/ksucalls.hpp"
#include "../defs.hpp"
#include "../hymo/mount/hymofs.hpp"
#include "../log.hpp"
#include "binder_wrapper.hpp"

#include <android/binder_parcel.h>
#include <fstream>

#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace ksud {
namespace murasaki {

using namespace hymo;

// 服务版本
static constexpr int32_t MURASAKI_VERSION = 1;

// Binder interface descriptor
static constexpr const char* INTERFACE_DESCRIPTOR = "io.murasaki.server.IMurasakiService";

namespace {

// Binder callbacks
static void* Binder_onCreate(void* args) {
    return args;
}

static void Binder_onDestroy(void* userData) {
    (void)userData;
}

// KernelSU allowlist binary format structures
// Matches kernel/app_profile.h
constexpr uint32_t KSU_ALLOWLIST_MAGIC = 0x7f4b5355;
constexpr uint32_t KSU_MAX_PACKAGE_NAME = 256;
constexpr int KSU_MAX_GROUPS = 32;
constexpr int KSU_SELINUX_DOMAIN = 64;

struct root_profile {
    int32_t uid;
    int32_t gid;
    int32_t groups_count;
    int32_t groups[KSU_MAX_GROUPS];
    struct {
        uint64_t effective;
        uint64_t permitted;
        uint64_t inheritable;
    } capabilities;
    char selinux_domain[KSU_SELINUX_DOMAIN];
    int32_t namespaces;
};

struct non_root_profile {
    uint8_t umount_modules;
};

struct app_profile {
    uint32_t version;
    char key[KSU_MAX_PACKAGE_NAME];
    int32_t current_uid;
    uint8_t allow_su;

    // The kernel structure alignment will insert padding here
    // to ensure the union (containing u64) is 8-byte aligned.
    // We rely on the compiler to match kernel layout.

    union {
        root_profile root;
        non_root_profile non_root;
    };
};

}  // namespace

// Helper: Check if uid is granted root via KernelSU
// Parses /data/adb/ksu/.allowlist directly to reuse KernelSU's authentication state
static bool is_uid_granted_root(uid_t uid) {
    if (uid == 0)
        return true;

    // TODO: Define this path in a common header
    const char* allowlist_path = "/data/adb/ksu/.allowlist";

    std::ifstream ifs(allowlist_path, std::ios::binary);
    if (!ifs) {
        LOGD("Murasaki: Failed to open allowlist at %s", allowlist_path);
        return false;
    }

    uint32_t magic;
    uint32_t version;

    if (!ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic)) ||
        !ifs.read(reinterpret_cast<char*>(&version), sizeof(version))) {
        return false;
    }

    if (magic != KSU_ALLOWLIST_MAGIC) {
        LOGD("Murasaki: Invalid allowlist magic");
        return false;
    }

    app_profile profile;
    while (ifs.read(reinterpret_cast<char*>(&profile), sizeof(profile))) {
        if (profile.current_uid == static_cast<int32_t>(uid)) {
            if (profile.allow_su) {
                return true;
            }
            return false;
        }
    }

    return false;
}

MurasakiBinderService& MurasakiBinderService::getInstance() {
    static MurasakiBinderService instance;
    return instance;
}

MurasakiBinderService::~MurasakiBinderService() {
    stop();
    if (binder_) {
        auto& bw = BinderWrapper::instance();
        if (bw.AIBinder_decStrong)
            bw.AIBinder_decStrong(binder_);
        binder_ = nullptr;
    }
    if (binderClass_) {
        // AIBinder_Class is managed by the system
        binderClass_ = nullptr;
    }
}

int MurasakiBinderService::init() {
    if (binder_) {
        LOGW("MurasakiBinderService already initialized");
        return 0;
    }

    LOGI("Initializing Murasaki Binder service...");

    // Initialize wrapper
    if (!BinderWrapper::instance().init()) {
        LOGE("Failed to initialize Binder wrapper");
        return -ENOSYS;
    }

    auto& bw = BinderWrapper::instance();

    // 检查必要的函数是否加载成功
    if (!bw.AIBinder_Class_define || !bw.AIBinder_new) {
        LOGE("Required binder functions not available");
        return -ENOSYS;
    }

    // 创建 Binder class
    binderClass_ =
        bw.AIBinder_Class_define(INTERFACE_DESCRIPTOR,
                                 Binder_onCreate,   // onCreate - NEW: required, pass-through args
                                 Binder_onDestroy,  // onDestroy
                                 onTransact);

    if (!binderClass_) {
        LOGE("Failed to define binder class");
        return -EINVAL;
    }

    // 创建 Binder 对象，传入 this 作为 user data
    binder_ = bw.AIBinder_new(binderClass_, this);
    if (!binder_) {
        LOGE("Failed to create binder");
        return -ENOMEM;
    }

    // 注册到 ServiceManager
    if (!bw.AServiceManager_addService) {
        LOGE("AServiceManager_addService not available");
        if (bw.AIBinder_decStrong)
            bw.AIBinder_decStrong(binder_);
        binder_ = nullptr;
        return -ENOSYS;
    }

    binder_status_t status = bw.AServiceManager_addService(binder_, MURASAKI_SERVICE_NAME);
    if (status != STATUS_OK) {
        LOGE("Failed to register service: %d", status);
        if (bw.AIBinder_decStrong)
            bw.AIBinder_decStrong(binder_);
        binder_ = nullptr;
        return -EPERM;
    }

    LOGI("Murasaki service registered as '%s'", MURASAKI_SERVICE_NAME);
    return 0;
}

void MurasakiBinderService::joinThreadPool() {
    running_ = true;
    LOGI("Joining Binder thread pool...");
    if (auto join = BinderWrapper::instance().ABinderProcess_joinThreadPool) {
        join();
    } else {
        LOGE("ABinderProcess_joinThreadPool not available");
    }
    running_ = false;
}

void MurasakiBinderService::startThreadPool() {
    if (running_) {
        return;
    }

    running_ = true;

    // 启动 Binder 线程
    if (auto start = BinderWrapper::instance().ABinderProcess_startThreadPool) {
        start();
    } else {
        LOGE("ABinderProcess_startThreadPool not available");
        // Not fatal, but service might not work
    }

    // 创建服务线程
    serviceThread_ = std::thread([this]() {
        LOGI("Murasaki service thread started");
        if (auto join = BinderWrapper::instance().ABinderProcess_joinThreadPool) {
            join();
        } else {
            LOGE("ABinderProcess_joinThreadPool not available");
        }
        LOGI("Murasaki service thread exited");
        running_ = false;
    });
    serviceThread_.detach();
}

void MurasakiBinderService::stop() {
    running_ = false;
    // Note: There's no clean way to stop the binder thread pool
    // The process will need to exit to stop it
}

uid_t MurasakiBinderService::getCallingUid() {
    auto& bw = BinderWrapper::instance();
    return bw.AIBinder_getCallingUid ? bw.AIBinder_getCallingUid() : 0;
}

bool MurasakiBinderService::checkCallerPermission(uid_t uid, int requiredLevel) {
    // Level 0 (SHELL): uid >= 2000 (shell uid)
    // Level 1 (ROOT): uid == 0 or granted root
    // Level 2 (KERNEL): uid == 0 and is manager or internal

    if (requiredLevel == 0) {
        return true;  // Anyone can call shell-level APIs
    }

    if (requiredLevel == 1) {
        return uid == 0 || is_uid_granted_root(uid);
    }

    if (requiredLevel == 2) {
        return uid == 0;  // Only root for kernel-level
    }

    return false;
}

// ==================== Transaction Handler ====================

binder_status_t MurasakiBinderService::onTransact(AIBinder* binder, transaction_code_t code,
                                                  const AParcel* in, AParcel* out) {
    auto& bw = BinderWrapper::instance();
    auto* service = static_cast<MurasakiBinderService*>(
        bw.AIBinder_getUserData ? bw.AIBinder_getUserData(binder) : nullptr);
    if (!service) {
        return STATUS_UNEXPECTED_NULL;
    }

    // Handle INTERFACE_TRANSACTION for Descriptor check
    if (code == 1598968902) {
        if (bw.AParcel_writeString)
            bw.AParcel_writeString(out, INTERFACE_DESCRIPTOR, -1);
        return STATUS_OK;
    }

    // Skip Interface Token
    int32_t strict_policy = 0;
    if (bw.AParcel_readInt32)
        bw.AParcel_readInt32(in, &strict_policy);
    std::string token;
    bw.readString(in, token);

    // IMurasakiService transactions
    switch (code) {
    case TRANSACTION_getVersion:
        return service->handleGetVersion(in, out);
    case TRANSACTION_getPrivilegeLevel:
        return service->handleGetPrivilegeLevel(in, out);
    case TRANSACTION_isKernelModeAvailable:
        return service->handleIsKernelModeAvailable(in, out);
    case TRANSACTION_getKernelSuVersion:
        return service->handleGetKsuVersion(in, out);
    case TRANSACTION_getSelinuxContext:
        return service->handleGetSelinuxContext(in, out);

        // Note: getHymoFsService/getKernelService would return sub-binder
        // For simplicity, we handle all transactions in one service for now
        // TODO: Implement proper sub-service architecture

    default:
        break;
    }

    LOGW("Unknown transaction code: %d", code);
    return STATUS_UNKNOWN_TRANSACTION;
}
// ==================== Handler Implementations ====================

// Helper macro to simplify AParcel calls through wrapper
#define BW BinderWrapper::instance()
// AIDL protocol: write status code (0 = success) before return value
#define WRITE_NO_EXCEPTION()   \
    if (BW.AParcel_writeInt32) \
    BW.AParcel_writeInt32(out, 0)

binder_status_t MurasakiBinderService::handleGetVersion(const AParcel* in, AParcel* out) {
    (void)in;
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeInt32(out, MURASAKI_VERSION);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleGetKsuVersion(const AParcel* in, AParcel* out) {
    (void)in;
    int32_t version = get_version();
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeInt32(out, version);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleGetPrivilegeLevel(const AParcel* in, AParcel* out) {
    (void)in;
    uid_t uid = getCallingUid();
    int32_t level = 0;  // SHELL

    if (uid == 0) {
        level = 2;  // KERNEL
    } else if (is_uid_granted_root(uid)) {
        level = 1;  // ROOT
    }

    WRITE_NO_EXCEPTION();
    BW.AParcel_writeInt32(out, level);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleIsKernelModeAvailable(const AParcel* in,
                                                                   AParcel* out) {
    (void)in;
    bool available = get_version() > 0;
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeBool(out, available);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleGetSelinuxContext(const AParcel* in, AParcel* out) {
    int32_t pid;
    BW.AParcel_readInt32(in, &pid);

    // Read from /proc/[pid]/attr/current
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/attr/current", pid == 0 ? getpid() : pid);

    FILE* f = fopen(path, "r");
    if (!f) {
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, "", 0);
        return STATUS_OK;
    }

    char context[256] = {0};
    fread(context, 1, sizeof(context) - 1, f);
    fclose(f);

    // Remove trailing newline
    size_t len = strlen(context);
    if (len > 0 && context[len - 1] == '\n') {
        context[len - 1] = '\0';
        len--;
    }

    WRITE_NO_EXCEPTION();
    BW.AParcel_writeString(out, context, len);
    return STATUS_OK;
}

// HymoFS handlers

binder_status_t MurasakiBinderService::handleHymoAddHideRule(const AParcel* in, AParcel* out) {
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    const char* path = nullptr;
    BW.AParcel_readString(in, &path, nullptr);

    int32_t targetUid;
    BW.AParcel_readInt32(in, &targetUid);

    bool result = HymoFS::hide_path(path ? path : "");
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoAddRedirectRule(const AParcel* in, AParcel* out) {
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    const char* src = nullptr;
    const char* target = nullptr;
    int32_t targetUid;

    BW.AParcel_readString(in, &src, nullptr);
    BW.AParcel_readString(in, &target, nullptr);
    BW.AParcel_readInt32(in, &targetUid);

    // type=0 is redirect rule
    bool result = HymoFS::add_rule(src ? src : "", target ? target : "", 0);
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoClearRules(const AParcel* in, AParcel* out) {
    (void)in;
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    bool result = HymoFS::clear_rules();
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoSetStealthMode(const AParcel* in, AParcel* out) {
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    bool enable;
    BW.AParcel_readBool(in, &enable);

    bool result = HymoFS::set_stealth(enable);
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoSetDebugMode(const AParcel* in, AParcel* out) {
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    bool enable;
    BW.AParcel_readBool(in, &enable);

    bool result = HymoFS::set_debug(enable);
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoGetActiveRules(const AParcel* in, AParcel* out) {
    (void)in;
    std::string rules = HymoFS::get_active_rules();
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeString(out, rules.c_str(), rules.size());
    return STATUS_OK;
}

// Kernel handlers

binder_status_t MurasakiBinderService::handleKernelIsUidGrantedRoot(const AParcel* in,
                                                                    AParcel* out) {
    int32_t uid;
    BW.AParcel_readInt32(in, &uid);

    bool granted = is_uid_granted_root(uid);
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeBool(out, granted);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleKernelNukeExt4Sysfs(const AParcel* in, AParcel* out) {
    (void)in;
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 2)) {
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    int result = nuke_ext4_sysfs("");
    WRITE_NO_EXCEPTION();
    BW.AParcel_writeInt32(out, result);
    return STATUS_OK;
}

#undef WRITE_NO_EXCEPTION
#undef BW

void MurasakiBinderService::onBinderDied(void* cookie) {
    (void)cookie;
    LOGW("Murasaki binder died");
}

// ==================== Public API ====================

void start_murasaki_binder_service_async() {
    std::thread([]() {
        auto& service = MurasakiBinderService::getInstance();

        if (service.init() != 0) {
            LOGE("Failed to initialize Murasaki Binder service");
            return;
        }

        // Start and join thread pool
        service.joinThreadPool();
    }).detach();
}

}  // namespace murasaki
}  // namespace ksud

#else  // !__ANDROID__

// Non-Android stub implementation
#include "../log.hpp"
#include "murasaki_binder.hpp"

namespace ksud {
namespace murasaki {

void start_murasaki_binder_service_async() {
    LOGW("Murasaki Binder service not available on this platform");
}

}  // namespace murasaki
}  // namespace ksud

#endif // #ifdef __ANDROID__
