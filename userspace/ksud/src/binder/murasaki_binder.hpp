// Murasaki Binder Service - Native Implementation
// 使用 libbinder_ndk 向 ServiceManager 注册服务
//
// 参考: https://source.android.com/docs/core/architecture/aidl/aidl-backends

#pragma once

#ifdef __ANDROID__
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
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

// Transaction codes (must match AIDL generated)
enum TransactionCode {
    TRANSACTION_getVersion = 1,
    TRANSACTION_getKsuVersion = 2,
    TRANSACTION_getPrivilegeLevel = 3,
    TRANSACTION_isKernelModeAvailable = 4,
    TRANSACTION_getSelinuxContext = 5,
    TRANSACTION_getHymoFsService = 6,
    TRANSACTION_getKernelService = 7,
    TRANSACTION_newProcess = 8,
    TRANSACTION_checkPermission = 9,

    // HymoFS transactions (100+)
    TRANSACTION_hymoAddHideRule = 100,
    TRANSACTION_hymoAddRedirectRule = 101,
    TRANSACTION_hymoRemoveRule = 102,
    TRANSACTION_hymoClearRules = 103,
    TRANSACTION_hymoSetStealthMode = 104,
    TRANSACTION_hymoIsStealthMode = 105,
    TRANSACTION_hymoSetDebugMode = 106,
    TRANSACTION_hymoIsDebugMode = 107,
    TRANSACTION_hymoSetMirrorPath = 108,
    TRANSACTION_hymoGetMirrorPath = 109,
    TRANSACTION_hymoFixMounts = 110,
    TRANSACTION_hymoGetActiveRules = 111,
    TRANSACTION_hymoGetStatus = 112,

    // Kernel transactions (200+)
    TRANSACTION_kernelGetAppProfile = 200,
    TRANSACTION_kernelSetAppProfile = 201,
    TRANSACTION_kernelIsUidGrantedRoot = 202,
    TRANSACTION_kernelShouldUmountForUid = 203,
    TRANSACTION_kernelInjectSepolicy = 204,
    TRANSACTION_kernelLoadSepolicyFromFile = 205,
    TRANSACTION_kernelAddTryUmount = 206,
    TRANSACTION_kernelNukeExt4Sysfs = 207,
    TRANSACTION_kernelRawIoctl = 208,
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