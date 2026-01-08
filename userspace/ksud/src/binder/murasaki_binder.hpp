// Murasaki Binder Service - Native Implementation
// 使用 libbinder_ndk 向 ServiceManager 注册服务
//
// 参考: https://source.android.com/docs/core/architecture/aidl/aidl-backends

#pragma once

#ifdef __ANDROID__
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>

#endif // #ifdef __ANDROID__

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace ksud {
namespace murasaki {

// 服务名称 - 用于 ServiceManager 注册
static constexpr const char* MURASAKI_SERVICE_NAME = "io.murasaki.IMurasakiService";

#ifdef __ANDROID__

// Transaction codes - MUST match AIDL definitions in Murasaki API
// See: aidl/io/murasaki/server/IMurasakiService.aidl
enum TransactionCode {
    // IMurasakiService (main)
    TRANSACTION_getVersion = 1,
    TRANSACTION_getPrivilegeLevel = 2,
    TRANSACTION_isKernelModeAvailable = 3,
    TRANSACTION_getKernelSuVersion = 4,
    TRANSACTION_getHymoFsService = 10,
    TRANSACTION_getKernelService = 11,
    TRANSACTION_getSelinuxContext = 20,
    TRANSACTION_setSelinuxContext = 21,
    TRANSACTION_requestPrivilegeLevel = 30,
    TRANSACTION_checkPrivilegeLevel = 31,

    // IHymoFsService - See: aidl/io/murasaki/server/IHymoFsService.aidl
    TRANSACTION_hymoGetProtocolVersion = 1,
    TRANSACTION_hymoIsAvailable = 2,
    TRANSACTION_hymoAddHideRule = 10,
    TRANSACTION_hymoAddRedirectRule = 11,
    TRANSACTION_hymoAddMergeRule = 12,
    TRANSACTION_hymoDeleteRule = 13,
    TRANSACTION_hymoClearAllRules = 14,
    TRANSACTION_hymoGetActiveRules = 15,
    TRANSACTION_hymoSetStealthMode = 20,
    TRANSACTION_hymoGetStealthMode = 21,
    TRANSACTION_hymoSetDebugMode = 22,
    TRANSACTION_hymoGetDebugMode = 23,
    TRANSACTION_hymoSetMirrorPath = 24,
    TRANSACTION_hymoGetMirrorPath = 25,
    TRANSACTION_hymoFixMounts = 30,
    TRANSACTION_hymoGetStatus = 31,

    // IKernelService - See: aidl/io/murasaki/server/IKernelService.aidl
    TRANSACTION_kernelGetAppProfile = 1,
    TRANSACTION_kernelSetAppProfile = 2,
    TRANSACTION_kernelIsUidGrantedRoot = 10,
    TRANSACTION_kernelShouldUmountForUid = 11,
    TRANSACTION_kernelInjectSepolicy = 20,
    TRANSACTION_kernelLoadSepolicyFromFile = 21,
    TRANSACTION_kernelAddTryUmount = 30,
    TRANSACTION_kernelClearTryUmount = 31,
    TRANSACTION_kernelNukeExt4Sysfs = 40,
    TRANSACTION_kernelRawIoctl = 50,
};

/**
 * Murasaki Binder Service
 *
 * 使用 NDK Binder API 实现真正的 Binder 服务
 */
class MurasakiBinderService {
public:
    static MurasakiBinderService& getInstance();

    /**
     * 初始化 Binder 服务
     * 创建 Binder 对象并注册到 ServiceManager
     * @return 0 成功，负数错误码
     */
    int init();

    /**
     * 启动 Binder 线程池
     * 会阻塞当前线程处理 Binder 事务
     */
    void joinThreadPool();

    /**
     * 启动 Binder 服务 (非阻塞)
     * 创建后台线程处理 Binder 事务
     */
    void startThreadPool();

    /**
     * 停止服务
     */
    void stop();

    /**
     * 检查服务是否运行中
     */
    bool isRunning() const { return running_.load(); }

    /**
     * 获取服务 Binder 对象
     */
    AIBinder* getBinder() const { return binder_; }

private:
    MurasakiBinderService() = default;
    ~MurasakiBinderService();

    // Binder transaction handler
    static binder_status_t onTransact(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                      AParcel* out);

    // Binder death handler
    static void onBinderDied(void* cookie);

    // 处理各种事务
    binder_status_t handleGetVersion(const AParcel* in, AParcel* out);
    binder_status_t handleGetKsuVersion(const AParcel* in, AParcel* out);
    binder_status_t handleGetPrivilegeLevel(const AParcel* in, AParcel* out);
    binder_status_t handleIsKernelModeAvailable(const AParcel* in, AParcel* out);
    binder_status_t handleGetSelinuxContext(const AParcel* in, AParcel* out);

    // HymoFS handlers
    binder_status_t handleHymoAddHideRule(const AParcel* in, AParcel* out);
    binder_status_t handleHymoAddRedirectRule(const AParcel* in, AParcel* out);
    binder_status_t handleHymoClearRules(const AParcel* in, AParcel* out);
    binder_status_t handleHymoSetStealthMode(const AParcel* in, AParcel* out);
    binder_status_t handleHymoSetDebugMode(const AParcel* in, AParcel* out);
    binder_status_t handleHymoGetActiveRules(const AParcel* in, AParcel* out);

    // Kernel handlers
    binder_status_t handleKernelIsUidGrantedRoot(const AParcel* in, AParcel* out);
    binder_status_t handleKernelNukeExt4Sysfs(const AParcel* in, AParcel* out);

    // 获取调用方 UID
    uid_t getCallingUid();

    // 检查权限
    bool checkCallerPermission(uid_t uid, int requiredLevel);

    AIBinder* binder_ = nullptr;
    AIBinder_Class* binderClass_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread serviceThread_;
};

#endif // #ifdef __ANDROID__

/**
 * 启动 Murasaki Binder 服务 (异步)
 * 仅 Android 平台可用
 */
void start_murasaki_binder_service_async();

}  // namespace murasaki
}  // namespace ksud