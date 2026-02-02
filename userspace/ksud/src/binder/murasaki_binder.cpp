// Murasaki Binder Service - Native Implementation
// 使用 libbinder_ndk 向 ServiceManager 注册服务
// Android-only: Uses libbinder_ndk API

#ifdef __ANDROID__

#include "murasaki_binder.hpp"
#include "../core/ksucalls.hpp"
#include "../defs.hpp"
#include "../hymo/mount/hymofs.hpp"
#include "../log.hpp"
#include "../sepolicy/sepolicy.hpp"
#include "../utils.hpp"
#include "binder_wrapper.hpp"
#include "shizuku_service.hpp"

#include <android/binder_parcel.h>
#include <fstream>

#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>

namespace ksud {
namespace murasaki {

using namespace hymo;

// 系统属性操作（与 Shizuku 兼容层保持一致）
extern "C" {
int __system_property_get(const char* name, char* value);
int __system_property_set(const char* name, const char* value);
}

// 服务版本
static constexpr int32_t MURASAKI_VERSION = 1;

// AIDL interface descriptors (MUST match murasaki-api/aidl)
static constexpr const char* DESCRIPTOR_MURASAKI = "io.murasaki.server.IMurasakiService";
static constexpr const char* DESCRIPTOR_HYMO = "io.murasaki.server.IHymoFsService";
static constexpr const char* DESCRIPTOR_KERNEL = "io.murasaki.server.IKernelService";
static constexpr const char* DESCRIPTOR_MODULE = "io.murasaki.server.IModuleService";

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

// ==================== 子服务：IHymoFsService / IKernelService / IModuleService ====================

static binder_status_t onTransactHymo(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                      AParcel* out);
static binder_status_t onTransactKernel(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                        AParcel* out);
static binder_status_t onTransactModule(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                        AParcel* out);

struct HymoRuleEntry {
    int32_t id;
    std::string a;
    std::string b;
    int32_t targetUid;
    int32_t flags;
};

static std::atomic<int32_t> g_rule_id{1};
static std::mutex g_rules_mutex;
static std::map<int32_t, HymoRuleEntry> g_hide_rules;
static std::map<int32_t, HymoRuleEntry> g_redirect_rules;
static std::atomic<bool> g_stealth{false};
static std::mutex g_uid_vis_mutex;
static std::map<int32_t, bool> g_uid_hidden;

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

    // Sub-binders are also owned by libbinder/servicemanager lifetime;
    // just drop strong refs.
    auto& bw = BinderWrapper::instance();
    if (hymoBinder_ && bw.AIBinder_decStrong) {
        bw.AIBinder_decStrong(hymoBinder_);
        hymoBinder_ = nullptr;
    }
    if (kernelBinder_ && bw.AIBinder_decStrong) {
        bw.AIBinder_decStrong(kernelBinder_);
        kernelBinder_ = nullptr;
    }
    if (moduleBinder_ && bw.AIBinder_decStrong) {
        bw.AIBinder_decStrong(moduleBinder_);
        moduleBinder_ = nullptr;
    }
    hymoClass_ = nullptr;
    kernelClass_ = nullptr;
    moduleClass_ = nullptr;
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
        bw.AIBinder_Class_define(DESCRIPTOR_MURASAKI,
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

    // 创建子服务 Binder（返回给客户端）
    hymoClass_ = bw.AIBinder_Class_define(DESCRIPTOR_HYMO, Binder_onCreate, Binder_onDestroy,
                                         onTransactHymo);
    kernelClass_ = bw.AIBinder_Class_define(DESCRIPTOR_KERNEL, Binder_onCreate, Binder_onDestroy,
                                           onTransactKernel);
    moduleClass_ = bw.AIBinder_Class_define(DESCRIPTOR_MODULE, Binder_onCreate, Binder_onDestroy,
                                           onTransactModule);

    if (hymoClass_) {
        hymoBinder_ = bw.AIBinder_new(hymoClass_, this);
    }
    if (kernelClass_) {
        kernelBinder_ = bw.AIBinder_new(kernelClass_, this);
    }
    if (moduleClass_) {
        moduleBinder_ = bw.AIBinder_new(moduleClass_, this);
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

bool MurasakiBinderService::isUidGrantedRoot(uid_t uid) {
    if (uid == 0)
        return true;

    const char* allowlist_path = "/data/adb/ksu/.allowlist";
    std::ifstream ifs(allowlist_path, std::ios::binary);
    if (!ifs) {
        return false;
    }

    uint32_t magic;
    uint32_t version;
    if (!ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic)) ||
        !ifs.read(reinterpret_cast<char*>(&version), sizeof(version))) {
        return false;
    }
    if (magic != KSU_ALLOWLIST_MAGIC) {
        return false;
    }

    app_profile profile;
    while (ifs.read(reinterpret_cast<char*>(&profile), sizeof(profile))) {
        if (profile.current_uid == static_cast<int32_t>(uid)) {
            return profile.allow_su != 0;
        }
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
            bw.AParcel_writeString(out, DESCRIPTOR_MURASAKI, -1);
        return STATUS_OK;
    }

    // Skip Interface Token
    int32_t strict_policy = 0;
    if (bw.AParcel_readInt32)
        bw.AParcel_readInt32(in, &strict_policy);
    std::string token;
    bw.readString(in, token);

    // Helper macro
#define BW BinderWrapper::instance()
#define WRITE_NO_EXCEPTION()   \
    if (BW.AParcel_writeInt32) \
    BW.AParcel_writeInt32(out, 0)

    // IMurasakiService.aidl (murasaki-api) transaction ids:
    // 1 getVersion
    // 2 getKernelSuVersion
    // 3 getUid
    // 4 getSELinuxContext
    // 10 getCallerPrivilegeLevel
    // 11 isUidGrantedRoot(int)
    // 12 grantRoot(int) [TODO]
    // 13 revokeRoot(int) [TODO]
    // 14 getRootUids() [TODO]
    // 20 getHymoFsService
    // 21 getKernelService
    // 22 getModuleService
    // 30 getShizukuBinder
    // 40 getSystemProperty(name, default)
    // 41 setSystemProperty(name, value)
    switch (code) {
    case 1: {  // getVersion()
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, MURASAKI_VERSION);
        return STATUS_OK;
    }
    case 2: {  // getKernelSuVersion()
        int32_t version = get_version();
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, version);
        return STATUS_OK;
    }
    case 3: {  // getUid()
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, static_cast<int32_t>(getuid()));
        return STATUS_OK;
    }
    case 4: {  // getSELinuxContext()
        char context[256] = {0};
        FILE* f = fopen("/proc/self/attr/current", "r");
        if (f) {
            fread(context, 1, sizeof(context) - 1, f);
            fclose(f);
            size_t len = strlen(context);
            if (len > 0 && context[len - 1] == '\n') {
                context[len - 1] = '\0';
            }
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, context, strlen(context));
        return STATUS_OK;
    }
    case 10: {  // getCallerPrivilegeLevel()
        uid_t uid = service->getCallingUid();
        int32_t level = 0;
        if (uid == 0) {
            level = 2;
        } else if (service->isUidGrantedRoot(uid)) {
            level = 1;
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, level);
        return STATUS_OK;
    }
    case 11: {  // isUidGrantedRoot(int uid)
        int32_t target_uid = 0;
        BW.AParcel_readInt32(in, &target_uid);
        bool granted = service->isUidGrantedRoot(static_cast<uid_t>(target_uid));
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, granted);
        return STATUS_OK;
    }
    case 12: {  // grantRoot(int uid) - TODO: implement persistent allowlist write
        (void)in;
        LOGW("grantRoot not implemented yet (return false)");
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, false);
        return STATUS_OK;
    }
    case 13: {  // revokeRoot(int uid) - TODO
        (void)in;
        LOGW("revokeRoot not implemented yet (return false)");
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, false);
        return STATUS_OK;
    }
    case 14: {  // getRootUids() - TODO (return empty array)
        WRITE_NO_EXCEPTION();
        // int[] format: length then elements
        BW.AParcel_writeInt32(out, 0);
        return STATUS_OK;
    }
    case 20: {  // getHymoFsService()
        uid_t uid = service->getCallingUid();
        bool allowed = (uid == 0) || service->isUidGrantedRoot(uid);
        if (!allowed) {
            // If not root, return null binder
            WRITE_NO_EXCEPTION();
            BW.AParcel_writeStrongBinder(out, nullptr);
            return STATUS_OK;
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeStrongBinder(out, service->hymoBinder_);
        return STATUS_OK;
    }
    case 21: {  // getKernelService()
        uid_t uid = service->getCallingUid();
        bool allowed = (uid == 0) || service->isUidGrantedRoot(uid);
        if (!allowed) {
            WRITE_NO_EXCEPTION();
            BW.AParcel_writeStrongBinder(out, nullptr);
            return STATUS_OK;
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeStrongBinder(out, service->kernelBinder_);
        return STATUS_OK;
    }
    case 22: {  // getModuleService()
        uid_t uid = service->getCallingUid();
        bool allowed = (uid == 0) || service->isUidGrantedRoot(uid);
        if (!allowed) {
            WRITE_NO_EXCEPTION();
            BW.AParcel_writeStrongBinder(out, nullptr);
            return STATUS_OK;
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeStrongBinder(out, service->moduleBinder_);
        return STATUS_OK;
    }
    case 30: {  // getShizukuBinder()
        // Return Shizuku compatible binder (if started)
        AIBinder* sb = ksud::shizuku::ShizukuService::getInstance().getBinder();
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeStrongBinder(out, sb);
        return STATUS_OK;
    }
    case 40: {  // getSystemProperty(String name, String defaultValue)
        std::string name;
        std::string def;
        BW.readString(in, name);
        BW.readString(in, def);
        std::string val = def;
        char buf[92] = {0};
        if (!name.empty() && __system_property_get(name.c_str(), buf) > 0) {
            val = buf;
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, val.c_str(), val.size());
        return STATUS_OK;
    }
    case 41: {  // setSystemProperty(String name, String value)
        std::string name;
        std::string value;
        BW.readString(in, name);
        BW.readString(in, value);
        uid_t uid = service->getCallingUid();
        if (uid != 0 && !service->isUidGrantedRoot(uid)) {
            // Permission denied: keep AIDL "no exception" but do nothing
            LOGW("setSystemProperty denied for uid %d", uid);
            WRITE_NO_EXCEPTION();
            return STATUS_OK;
        }
        if (!name.empty()) {
            __system_property_set(name.c_str(), value.c_str());
        }
        WRITE_NO_EXCEPTION();
        return STATUS_OK;
    }
    default:
        LOGW("Unknown IMurasakiService transaction: %d (token=%s)", code, token.c_str());
        return STATUS_UNKNOWN_TRANSACTION;
    }

#undef WRITE_NO_EXCEPTION
#undef BW
}

void MurasakiBinderService::onBinderDied(void* cookie) {
    (void)cookie;
    LOGW("Murasaki binder died");
}

// ==================== IHymoFsService implementation ====================

static binder_status_t onTransactHymo(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                      AParcel* out) {
    auto& bw = BinderWrapper::instance();
    auto* svc = static_cast<MurasakiBinderService*>(
        bw.AIBinder_getUserData ? bw.AIBinder_getUserData(binder) : nullptr);
    if (!svc) {
        return STATUS_UNEXPECTED_NULL;
    }

    if (code == 1598968902) {
        if (bw.AParcel_writeString)
            bw.AParcel_writeString(out, DESCRIPTOR_HYMO, -1);
        return STATUS_OK;
    }

    // Skip Interface Token
    int32_t strict_policy = 0;
    if (bw.AParcel_readInt32)
        bw.AParcel_readInt32(in, &strict_policy);
    std::string token;
    bw.readString(in, token);

#define BW BinderWrapper::instance()
#define WRITE_NO_EXCEPTION()   \
    if (BW.AParcel_writeInt32) \
    BW.AParcel_writeInt32(out, 0)

    uid_t caller = svc->getCallingUid();
    bool allowed = (caller == 0) || svc->isUidGrantedRoot(caller);
    if (!allowed) {
        // For all mutating APIs, deny; for readonly, return safe defaults
        switch (code) {
        case 1:  // getVersion
        case 2:  // isAvailable
        case 3:  // isStealthMode
        case 24: // getHideRules
        case 34: // getRedirectRules
            break;
        default:
            WRITE_NO_EXCEPTION();
            // Return "failure" shape depending on signature
            if (code == 10 || code == 22 || code == 32 || code == 40 || code == 41) {
                BW.AParcel_writeBool(out, false);
            } else if (code == 20 || code == 21 || code == 30 || code == 31) {
                BW.AParcel_writeInt32(out, -1);
            } else {
                // void or unknown
            }
            return STATUS_OK;
        }
    }

    switch (code) {
    case 1: {  // int getVersion()
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, HymoFS::get_protocol_version());
        return STATUS_OK;
    }
    case 2: {  // boolean isAvailable()
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, HymoFS::is_available());
        return STATUS_OK;
    }
    case 3: {  // boolean isStealthMode()
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, g_stealth.load());
        return STATUS_OK;
    }
    case 10: {  // boolean setStealthMode(boolean enabled)
        bool enabled = false;
        BW.AParcel_readBool(in, &enabled);
        bool ok = HymoFS::set_stealth(enabled);
        if (ok) {
            g_stealth.store(enabled);
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, ok);
        return STATUS_OK;
    }
    case 20: {  // int addHideRule(String path)
        std::string path;
        BW.readString(in, path);
        bool ok = HymoFS::hide_path(path);
        int32_t id = -1;
        if (ok) {
            id = g_rule_id.fetch_add(1);
            std::lock_guard<std::mutex> lock(g_rules_mutex);
            g_hide_rules[id] = HymoRuleEntry{.id = id, .a = path, .b = "", .targetUid = 0, .flags = 0};
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, id);
        return STATUS_OK;
    }
    case 21: {  // int addHideRuleForUid(String path, int targetUid)
        std::string path;
        BW.readString(in, path);
        int32_t targetUid = 0;
        BW.AParcel_readInt32(in, &targetUid);
        bool ok = HymoFS::hide_path(path);
        int32_t id = -1;
        if (ok) {
            id = g_rule_id.fetch_add(1);
            std::lock_guard<std::mutex> lock(g_rules_mutex);
            g_hide_rules[id] =
                HymoRuleEntry{.id = id, .a = path, .b = "", .targetUid = targetUid, .flags = 0};
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, id);
        return STATUS_OK;
    }
    case 22: {  // boolean removeHideRule(int ruleId)
        int32_t ruleId = -1;
        BW.AParcel_readInt32(in, &ruleId);
        std::string path;
        {
            std::lock_guard<std::mutex> lock(g_rules_mutex);
            auto it = g_hide_rules.find(ruleId);
            if (it != g_hide_rules.end()) {
                path = it->second.a;
            }
        }
        bool ok = false;
        if (!path.empty()) {
            ok = HymoFS::delete_rule(path);
            if (ok) {
                std::lock_guard<std::mutex> lock(g_rules_mutex);
                g_hide_rules.erase(ruleId);
            }
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, ok);
        return STATUS_OK;
    }
    case 23: {  // void clearHideRules()
        (void)HymoFS::clear_rules();
        std::lock_guard<std::mutex> lock(g_rules_mutex);
        g_hide_rules.clear();
        g_redirect_rules.clear();
        WRITE_NO_EXCEPTION();
        return STATUS_OK;
    }
    case 24: {  // String[] getHideRules()
        std::lock_guard<std::mutex> lock(g_rules_mutex);
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, static_cast<int32_t>(g_hide_rules.size()));
        for (const auto& [id, e] : g_hide_rules) {
            std::string s = std::to_string(id) + ":" + e.a + ":" + std::to_string(e.targetUid);
            BW.AParcel_writeString(out, s.c_str(), static_cast<int32_t>(s.size()));
        }
        return STATUS_OK;
    }
    case 30: {  // int addRedirectRule(String sourcePath, String targetPath, int flags)
        std::string src, dst;
        BW.readString(in, src);
        BW.readString(in, dst);
        int32_t flags = 0;
        BW.AParcel_readInt32(in, &flags);
        bool ok = HymoFS::add_rule(src, dst, 0);
        int32_t id = -1;
        if (ok) {
            id = g_rule_id.fetch_add(1);
            std::lock_guard<std::mutex> lock(g_rules_mutex);
            g_redirect_rules[id] = HymoRuleEntry{.id = id, .a = src, .b = dst, .targetUid = 0, .flags = flags};
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, id);
        return STATUS_OK;
    }
    case 31: {  // int addRedirectRuleForUid(String sourcePath, String targetPath, int targetUid, int flags)
        std::string src, dst;
        BW.readString(in, src);
        BW.readString(in, dst);
        int32_t targetUid = 0;
        int32_t flags = 0;
        BW.AParcel_readInt32(in, &targetUid);
        BW.AParcel_readInt32(in, &flags);
        bool ok = HymoFS::add_rule(src, dst, 0);
        int32_t id = -1;
        if (ok) {
            id = g_rule_id.fetch_add(1);
            std::lock_guard<std::mutex> lock(g_rules_mutex);
            g_redirect_rules[id] =
                HymoRuleEntry{.id = id, .a = src, .b = dst, .targetUid = targetUid, .flags = flags};
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, id);
        return STATUS_OK;
    }
    case 32: {  // boolean removeRedirectRule(int ruleId)
        int32_t ruleId = -1;
        BW.AParcel_readInt32(in, &ruleId);
        std::string src;
        {
            std::lock_guard<std::mutex> lock(g_rules_mutex);
            auto it = g_redirect_rules.find(ruleId);
            if (it != g_redirect_rules.end()) {
                src = it->second.a;
            }
        }
        bool ok = false;
        if (!src.empty()) {
            ok = HymoFS::delete_rule(src);
            if (ok) {
                std::lock_guard<std::mutex> lock(g_rules_mutex);
                g_redirect_rules.erase(ruleId);
            }
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, ok);
        return STATUS_OK;
    }
    case 33: {  // void clearRedirectRules()
        (void)HymoFS::clear_rules();
        std::lock_guard<std::mutex> lock(g_rules_mutex);
        g_hide_rules.clear();
        g_redirect_rules.clear();
        WRITE_NO_EXCEPTION();
        return STATUS_OK;
    }
    case 34: {  // String[] getRedirectRules()
        std::lock_guard<std::mutex> lock(g_rules_mutex);
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, static_cast<int32_t>(g_redirect_rules.size()));
        for (const auto& [id, e] : g_redirect_rules) {
            std::string s = std::to_string(id) + ":" + e.a + ":" + e.b + ":" +
                            std::to_string(e.targetUid) + ":" + std::to_string(e.flags);
            BW.AParcel_writeString(out, s.c_str(), static_cast<int32_t>(s.size()));
        }
        return STATUS_OK;
    }
    case 40: {  // boolean bindMount(String source, String target, int flags) - TODO
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, false);
        return STATUS_OK;
    }
    case 41: {  // boolean umount(String path) - TODO
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, false);
        return STATUS_OK;
    }
    case 50: {  // void setUidVisibility(int uid, boolean hidden)
        int32_t uid = 0;
        bool hidden = false;
        BW.AParcel_readInt32(in, &uid);
        BW.AParcel_readBool(in, &hidden);
        {
            std::lock_guard<std::mutex> lock(g_uid_vis_mutex);
            g_uid_hidden[uid] = hidden;
        }
        WRITE_NO_EXCEPTION();
        return STATUS_OK;
    }
    case 51: {  // boolean isUidHidden(int uid)
        int32_t uid = 0;
        BW.AParcel_readInt32(in, &uid);
        bool hidden = false;
        {
            std::lock_guard<std::mutex> lock(g_uid_vis_mutex);
            auto it = g_uid_hidden.find(uid);
            hidden = it != g_uid_hidden.end() ? it->second : false;
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, hidden);
        return STATUS_OK;
    }
    default:
        LOGW("Unknown IHymoFsService transaction: %d (token=%s)", code, token.c_str());
        return STATUS_UNKNOWN_TRANSACTION;
    }

#undef WRITE_NO_EXCEPTION
#undef BW
}

// ==================== IKernelService implementation ====================

static int read_pid_uid(int32_t pid) {
    auto content = ksud::read_file("/proc/" + std::to_string(pid) + "/status");
    if (!content)
        return -1;
    for (const auto& line : ksud::split(*content, '\n')) {
        if (ksud::starts_with(line, "Uid:")) {
            // Format: Uid: real effective saved fs
            auto parts = ksud::split(line, '\t');
            for (const auto& p : parts) {
                if (!p.empty() && p[0] >= '0' && p[0] <= '9') {
                    return atoi(p.c_str());
                }
            }
        }
    }
    return -1;
}

static binder_status_t onTransactKernel(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                        AParcel* out) {
    auto& bw = BinderWrapper::instance();
    auto* svc = static_cast<MurasakiBinderService*>(
        bw.AIBinder_getUserData ? bw.AIBinder_getUserData(binder) : nullptr);
    if (!svc) {
        return STATUS_UNEXPECTED_NULL;
    }

    if (code == 1598968902) {
        if (bw.AParcel_writeString)
            bw.AParcel_writeString(out, DESCRIPTOR_KERNEL, -1);
        return STATUS_OK;
    }

    int32_t strict_policy = 0;
    if (bw.AParcel_readInt32)
        bw.AParcel_readInt32(in, &strict_policy);
    std::string token;
    bw.readString(in, token);

#define BW BinderWrapper::instance()
#define WRITE_NO_EXCEPTION()   \
    if (BW.AParcel_writeInt32) \
    BW.AParcel_writeInt32(out, 0)

    uid_t caller = svc->getCallingUid();
    bool allowed = (caller == 0) || svc->isUidGrantedRoot(caller);
    if (!allowed) {
        WRITE_NO_EXCEPTION();
        // Return safe defaults for common return types
        if (code == 1 || code == 10 || code == 21) {
            BW.AParcel_writeInt32(out, -1);
        } else if (code == 11 || code == 13 || code == 20 || code == 41 || code == 50 || code == 51 ||
                   code == 60 || code == 61) {
            BW.AParcel_writeBool(out, false);
        } else if (code == 2 || code == 12 || code == 40 || code == 62) {
            BW.AParcel_writeString(out, "", 0);
        }
        return STATUS_OK;
    }

    switch (code) {
    case 1: {  // int getVersion()
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, 1);
        return STATUS_OK;
    }
    case 2: {  // String getKernelVersion()
        std::string ver;
        if (auto s = ksud::read_file("/proc/sys/kernel/osrelease")) {
            ver = ksud::trim(*s);
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, ver.c_str(), static_cast<int32_t>(ver.size()));
        return STATUS_OK;
    }
    case 10: {  // int getSELinuxEnforce()
        // 0=Disabled,1=Permissive,2=Enforcing
        int32_t mode = 0;
        if (auto s = ksud::read_file("/sys/fs/selinux/enforce")) {
            std::string v = ksud::trim(*s);
            if (!v.empty()) {
                mode = (v[0] == '1') ? 2 : 1;
            }
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, mode);
        return STATUS_OK;
    }
    case 11: {  // boolean setSELinuxEnforce(boolean enforce)
        bool enforce = false;
        BW.AParcel_readBool(in, &enforce);
        bool ok = ksud::write_file("/sys/fs/selinux/enforce", enforce ? "1" : "0");
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, ok);
        return STATUS_OK;
    }
    case 12: {  // String getProcessContext(int pid)
        int32_t pid = 0;
        BW.AParcel_readInt32(in, &pid);
        std::string ctx;
        auto p = "/proc/" + std::to_string(pid) + "/attr/current";
        if (auto s = ksud::read_file(p)) {
            ctx = ksud::trim(*s);
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, ctx.c_str(), static_cast<int32_t>(ctx.size()));
        return STATUS_OK;
    }
    case 13: {  // boolean injectSepolicy(String rule)
        std::string rule;
        BW.readString(in, rule);
        int ret = sepolicy_live_patch(rule);
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, ret == 0);
        return STATUS_OK;
    }
    case 14: {  // int injectSepolicyBatch(String[] rules)
        int32_t n = 0;
        BW.AParcel_readInt32(in, &n);
        int32_t ok_count = 0;
        for (int32_t i = 0; i < n; i++) {
            std::string rule;
            BW.readString(in, rule);
            if (sepolicy_live_patch(rule) == 0) {
                ok_count++;
            }
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, ok_count);
        return STATUS_OK;
    }
    case 20: {  // boolean killProcess(int pid, int signal)
        int32_t pid = 0, sig = 9;
        BW.AParcel_readInt32(in, &pid);
        BW.AParcel_readInt32(in, &sig);
        bool ok = (kill(pid, sig) == 0);
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, ok);
        return STATUS_OK;
    }
    case 21: {  // int getProcessUid(int pid)
        int32_t pid = 0;
        BW.AParcel_readInt32(in, &pid);
        int uid = read_pid_uid(pid);
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, uid);
        return STATUS_OK;
    }
    case 40: {  // String readSysctl(String name)
        std::string name;
        BW.readString(in, name);
        for (auto& c : name) {
            if (c == '.')
                c = '/';
        }
        std::string val;
        if (auto s = ksud::read_file("/proc/sys/" + name)) {
            val = ksud::trim(*s);
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, val.c_str(), static_cast<int32_t>(val.size()));
        return STATUS_OK;
    }
    case 41: {  // boolean writeSysctl(String name, String value)
        std::string name, value;
        BW.readString(in, name);
        BW.readString(in, value);
        for (auto& c : name) {
            if (c == '.')
                c = '/';
        }
        bool ok = ksud::write_file("/proc/sys/" + name, value);
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, ok);
        return STATUS_OK;
    }
    case 62: {  // String execCommand(String command)
        std::string cmd;
        BW.readString(in, cmd);
        auto r = ksud::exec_command({"sh", "-c", cmd});
        std::string out_str = r.stdout_str;
        if (!r.stderr_str.empty()) {
            out_str += r.stderr_str;
        }
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, out_str.c_str(), static_cast<int32_t>(out_str.size()));
        return STATUS_OK;
    }
    default:
        LOGW("Unknown IKernelService transaction: %d (token=%s)", code, token.c_str());
        return STATUS_UNKNOWN_TRANSACTION;
    }

#undef WRITE_NO_EXCEPTION
#undef BW
}

// ==================== IModuleService implementation (stub) ====================

static binder_status_t onTransactModule(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                        AParcel* out) {
    auto& bw = BinderWrapper::instance();
    auto* svc = static_cast<MurasakiBinderService*>(
        bw.AIBinder_getUserData ? bw.AIBinder_getUserData(binder) : nullptr);
    if (!svc) {
        return STATUS_UNEXPECTED_NULL;
    }

    if (code == 1598968902) {
        if (bw.AParcel_writeString)
            bw.AParcel_writeString(out, DESCRIPTOR_MODULE, -1);
        return STATUS_OK;
    }

    int32_t strict_policy = 0;
    if (bw.AParcel_readInt32)
        bw.AParcel_readInt32(in, &strict_policy);
    std::string token;
    bw.readString(in, token);

#define BW BinderWrapper::instance()
#define WRITE_NO_EXCEPTION()   \
    if (BW.AParcel_writeInt32) \
    BW.AParcel_writeInt32(out, 0)

    uid_t caller = svc->getCallingUid();
    bool allowed = (caller == 0) || svc->isUidGrantedRoot(caller);
    if (!allowed) {
        WRITE_NO_EXCEPTION();
        if (code == 1) {
            BW.AParcel_writeInt32(out, -1);
        } else if (code == 10) {
            BW.AParcel_writeInt32(out, 0);  // empty String[] length
        } else if (code == 12 || code == 20) {
            BW.AParcel_writeBool(out, false);
        } else if (code == 11 || code == 40 || code == 41) {
            BW.AParcel_writeString(out, "", 0);
        } else if (code == 30) {
            BW.AParcel_writeInt32(out, -1);
        } else if (code == 21 || code == 22 || code == 31) {
            BW.AParcel_writeBool(out, false);
        }
        return STATUS_OK;
    }

    switch (code) {
    case 1: {  // int getVersion()
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, 1);
        return STATUS_OK;
    }
    case 10: {  // String[] getInstalledModules()
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, 0);
        return STATUS_OK;
    }
    case 11: {  // String getModuleInfo(String moduleId)
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, "{}", 2);
        return STATUS_OK;
    }
    case 12: {  // boolean isModuleInstalled(String moduleId)
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, false);
        return STATUS_OK;
    }
    case 20: {  // boolean isModuleEnabled(String moduleId)
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, false);
        return STATUS_OK;
    }
    case 21: {  // boolean enableModule(String moduleId)
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, false);
        return STATUS_OK;
    }
    case 22: {  // boolean disableModule(String moduleId)
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, false);
        return STATUS_OK;
    }
    case 30: {  // int installModule(String zipPath)
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeInt32(out, -ENOSYS);
        return STATUS_OK;
    }
    case 31: {  // boolean uninstallModule(String moduleId)
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeBool(out, false);
        return STATUS_OK;
    }
    case 40: {  // String runModuleAction(String moduleId, String action)
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, "", 0);
        return STATUS_OK;
    }
    case 41: {  // String getModuleWebUIUrl(String moduleId)
        (void)in;
        WRITE_NO_EXCEPTION();
        BW.AParcel_writeString(out, "", 0);
        return STATUS_OK;
    }
    default:
        LOGW("Unknown IModuleService transaction: %d (token=%s)", code, token.c_str());
        return STATUS_UNKNOWN_TRANSACTION;
    }

#undef WRITE_NO_EXCEPTION
#undef BW
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
