// Murasaki Binder Service - Native Implementation
// 使用 libbinder_ndk 向 ServiceManager 注册服务
//
// 参考: https://source.android.com/docs/core/architecture/aidl/aidl-backends

#pragma once

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>

#include <atomic>
#include <string>
#include <thread>

namespace ksud {
namespace murasaki {

// ServiceManager 注册名（客户端 Murasaki.java 直连用这个 name）
static constexpr const char* MURASAKI_SERVICE_NAME = "io.murasaki.IMurasakiService";

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

    // 获取调用方 UID
    uid_t getCallingUid();

    // 检查权限
    bool isUidGrantedRoot(uid_t uid);

    AIBinder* binder_ = nullptr;
    AIBinder_Class* binderClass_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread serviceThread_;

    // 子服务 Binder（由主服务返回给客户端）
    AIBinder* hymoBinder_ = nullptr;
    AIBinder_Class* hymoClass_ = nullptr;
    AIBinder* kernelBinder_ = nullptr;
    AIBinder_Class* kernelClass_ = nullptr;
    AIBinder* moduleBinder_ = nullptr;
    AIBinder_Class* moduleClass_ = nullptr;
};

/**
 * 启动 Murasaki Binder 服务 (异步)
 * 仅 Android 平台可用
 */
void start_murasaki_binder_service_async();

}  // namespace murasaki
}  // namespace ksud