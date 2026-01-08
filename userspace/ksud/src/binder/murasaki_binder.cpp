// Murasaki Binder Service - Native Implementation
// 使用 libbinder_ndk 向 ServiceManager 注册服务
// Android-only: Uses libbinder_ndk API

#ifdef __ANDROID__

#include "murasaki_binder.hpp"
#include "../core/ksucalls.hpp"
#include "../defs.hpp"
#include "../hymo/mount/hymofs.hpp"
#include "../log.hpp"

#include <android/binder_parcel.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace ksud {
namespace murasaki {

using namespace hymo;

// 服务版本
static constexpr int32_t MURASAKI_VERSION = 1;

// Binder interface descriptor
static constexpr const char* INTERFACE_DESCRIPTOR = "io.murasaki.IMurasakiService";

// Helper: Check if uid is granted root via KernelSU
// Uses mark system to check permissions
static bool is_uid_granted_root(uid_t uid) {
    if (uid == 0)
        return true;

    // For now, we can't directly check uid->root mapping without pid
    // The kernel manages uid allowlist internally
    // TODO: Add proper uid checking via new ioctl if needed
    return false;
}

MurasakiBinderService& MurasakiBinderService::getInstance() {
    static MurasakiBinderService instance;
    return instance;
}

MurasakiBinderService::~MurasakiBinderService() {
    stop();
    if (binder_) {
        AIBinder_decStrong(binder_);
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

    // 创建 Binder class
    binderClass_ = AIBinder_Class_define(INTERFACE_DESCRIPTOR,
                                         nullptr,  // onCreate - we manage instance ourselves
                                         nullptr,  // onDestroy
                                         onTransact);

    if (!binderClass_) {
        LOGE("Failed to define binder class");
        return -EINVAL;
    }

    // 创建 Binder 对象，传入 this 作为 user data
    binder_ = AIBinder_new(binderClass_, this);
    if (!binder_) {
        LOGE("Failed to create binder");
        return -ENOMEM;
    }

    // 注册到 ServiceManager
    binder_status_t status = AServiceManager_addService(binder_, MURASAKI_SERVICE_NAME);
    if (status != STATUS_OK) {
        LOGE("Failed to register service: %d", status);
        AIBinder_decStrong(binder_);
        binder_ = nullptr;
        return -EPERM;
    }

    LOGI("Murasaki service registered as '%s'", MURASAKI_SERVICE_NAME);
    return 0;
}

void MurasakiBinderService::joinThreadPool() {
    running_ = true;
    LOGI("Joining Binder thread pool...");
    ABinderProcess_joinThreadPool();
    running_ = false;
}

void MurasakiBinderService::startThreadPool() {
    if (running_) {
        return;
    }

    running_ = true;

    // 启动 Binder 线程
    ABinderProcess_startThreadPool();

    // 创建服务线程
    serviceThread_ = std::thread([this]() {
        LOGI("Murasaki service thread started");
        ABinderProcess_joinThreadPool();
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
    return AIBinder_getCallingUid();
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
    auto* service = static_cast<MurasakiBinderService*>(AIBinder_getUserData(binder));
    if (!service) {
        return STATUS_UNEXPECTED_NULL;
    }

    switch (code) {
    case TRANSACTION_getVersion:
        return service->handleGetVersion(in, out);
    case TRANSACTION_getKsuVersion:
        return service->handleGetKsuVersion(in, out);
    case TRANSACTION_getPrivilegeLevel:
        return service->handleGetPrivilegeLevel(in, out);
    case TRANSACTION_isKernelModeAvailable:
        return service->handleIsKernelModeAvailable(in, out);
    case TRANSACTION_getSelinuxContext:
        return service->handleGetSelinuxContext(in, out);

    // HymoFS
    case TRANSACTION_hymoAddHideRule:
        return service->handleHymoAddHideRule(in, out);
    case TRANSACTION_hymoAddRedirectRule:
        return service->handleHymoAddRedirectRule(in, out);
    case TRANSACTION_hymoClearRules:
        return service->handleHymoClearRules(in, out);
    case TRANSACTION_hymoSetStealthMode:
        return service->handleHymoSetStealthMode(in, out);
    case TRANSACTION_hymoSetDebugMode:
        return service->handleHymoSetDebugMode(in, out);
    case TRANSACTION_hymoGetActiveRules:
        return service->handleHymoGetActiveRules(in, out);

    // Kernel
    case TRANSACTION_kernelIsUidGrantedRoot:
        return service->handleKernelIsUidGrantedRoot(in, out);
    case TRANSACTION_kernelNukeExt4Sysfs:
        return service->handleKernelNukeExt4Sysfs(in, out);

    default:
        LOGW("Unknown transaction code: %d", code);
        return STATUS_UNKNOWN_TRANSACTION;
    }
}

// ==================== Handler Implementations ====================

binder_status_t MurasakiBinderService::handleGetVersion(const AParcel* in, AParcel* out) {
    (void)in;
    AParcel_writeInt32(out, MURASAKI_VERSION);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleGetKsuVersion(const AParcel* in, AParcel* out) {
    (void)in;
    int32_t version = get_version();
    AParcel_writeInt32(out, version);
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

    AParcel_writeInt32(out, level);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleIsKernelModeAvailable(const AParcel* in,
                                                                   AParcel* out) {
    (void)in;
    bool available = get_version() > 0;
    AParcel_writeBool(out, available);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleGetSelinuxContext(const AParcel* in, AParcel* out) {
    int32_t pid;
    AParcel_readInt32(in, &pid);

    // Read from /proc/[pid]/attr/current
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/attr/current", pid == 0 ? getpid() : pid);

    FILE* f = fopen(path, "r");
    if (!f) {
        AParcel_writeString(out, "", 0);
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

    AParcel_writeString(out, context, len);
    return STATUS_OK;
}

// HymoFS handlers

binder_status_t MurasakiBinderService::handleHymoAddHideRule(const AParcel* in, AParcel* out) {
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    const char* path = nullptr;
    AParcel_readString(in, &path, nullptr);

    int32_t targetUid;
    AParcel_readInt32(in, &targetUid);

    bool result = HymoFS::hide_path(path ? path : "");
    AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoAddRedirectRule(const AParcel* in, AParcel* out) {
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    const char* src = nullptr;
    const char* target = nullptr;
    int32_t targetUid;

    AParcel_readString(in, &src, nullptr);
    AParcel_readString(in, &target, nullptr);
    AParcel_readInt32(in, &targetUid);

    // type=0 is redirect rule
    bool result = HymoFS::add_rule(src ? src : "", target ? target : "", 0);
    AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoClearRules(const AParcel* in, AParcel* out) {
    (void)in;
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    bool result = HymoFS::clear_rules();
    AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoSetStealthMode(const AParcel* in, AParcel* out) {
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    bool enable;
    AParcel_readBool(in, &enable);

    bool result = HymoFS::set_stealth(enable);
    AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoSetDebugMode(const AParcel* in, AParcel* out) {
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 1)) {
        AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    bool enable;
    AParcel_readBool(in, &enable);

    bool result = HymoFS::set_debug(enable);
    AParcel_writeInt32(out, result ? 0 : -1);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleHymoGetActiveRules(const AParcel* in, AParcel* out) {
    (void)in;
    std::string rules = HymoFS::get_active_rules();
    AParcel_writeString(out, rules.c_str(), rules.size());
    return STATUS_OK;
}

// Kernel handlers

binder_status_t MurasakiBinderService::handleKernelIsUidGrantedRoot(const AParcel* in,
                                                                    AParcel* out) {
    int32_t uid;
    AParcel_readInt32(in, &uid);

    bool granted = is_uid_granted_root(uid);
    AParcel_writeBool(out, granted);
    return STATUS_OK;
}

binder_status_t MurasakiBinderService::handleKernelNukeExt4Sysfs(const AParcel* in, AParcel* out) {
    (void)in;
    uid_t uid = getCallingUid();
    if (!checkCallerPermission(uid, 2)) {
        AParcel_writeInt32(out, -EPERM);
        return STATUS_OK;
    }

    int result = nuke_ext4_sysfs("");
    AParcel_writeInt32(out, result);
    return STATUS_OK;
}

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
