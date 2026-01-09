// Shizuku 兼容服务 - C++ 原生实现
// 实现 moe.shizuku.server.IShizukuService 接口
// 让现有 Shizuku/Sui 生态的 App 可以直接使用

#pragma once

#include <android/binder_ibinder.h>
#include <sys/types.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ksud {
namespace shizuku {

// Shizuku API 版本 - 匹配官方 Shizuku
static constexpr int32_t SHIZUKU_SERVER_VERSION = 15;

// 服务描述符 - 必须与 AIDL 一致
static constexpr const char* SHIZUKU_DESCRIPTOR = "moe.shizuku.server.IShizukuService";
static constexpr const char* REMOTE_PROCESS_DESCRIPTOR = "moe.shizuku.server.IRemoteProcess";

// IShizukuService transaction codes - 来自 AIDL 定义
enum ShizukuTransactionCode {
    TRANSACTION_getVersion = 2,
    TRANSACTION_getUid = 3,
    TRANSACTION_checkPermission = 4,
    TRANSACTION_newProcess = 7,
    TRANSACTION_getSELinuxContext = 8,
    TRANSACTION_getSystemProperty = 9,
    TRANSACTION_setSystemProperty = 10,
    TRANSACTION_addUserService = 11,
    TRANSACTION_removeUserService = 12,
    TRANSACTION_requestPermission = 14,
    TRANSACTION_checkSelfPermission = 15,
    TRANSACTION_shouldShowRequestPermissionRationale = 16,
    TRANSACTION_attachApplication = 17,
    TRANSACTION_exit = 100,
    TRANSACTION_attachUserService = 101,
    TRANSACTION_dispatchPackageChanged = 102,
    TRANSACTION_isHidden = 103,
    TRANSACTION_dispatchPermissionConfirmationResult = 104,
    TRANSACTION_getFlagsForUid = 105,
    TRANSACTION_updateFlagsForUid = 106,
};

// IRemoteProcess transaction codes
enum RemoteProcessTransactionCode {
    TRANSACTION_getOutputStream = 1,
    TRANSACTION_getInputStream = 2,
    TRANSACTION_getErrorStream = 3,
    TRANSACTION_waitFor = 4,
    TRANSACTION_exitValue = 5,
    TRANSACTION_destroy = 6,
    TRANSACTION_alive = 7,
    TRANSACTION_waitForTimeout = 8,
};

// 远程进程持有者 - 实现 IRemoteProcess
class RemoteProcessHolder {
public:
    RemoteProcessHolder(pid_t pid, int stdin_fd, int stdout_fd, int stderr_fd);
    ~RemoteProcessHolder();

    // 获取文件描述符
    int getOutputStream();  // stdin (写入到进程)
    int getInputStream();   // stdout (从进程读取)
    int getErrorStream();   // stderr

    // 进程控制
    int waitFor();
    int exitValue();
    void destroy();
    bool alive();
    bool waitForTimeout(int64_t timeout_ms);

    // Binder 处理
    AIBinder* getBinder();
    static binder_status_t onTransact(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                      AParcel* out);

private:
    pid_t pid_;
    int stdin_fd_;   // 写入到进程的 stdin
    int stdout_fd_;  // 从进程的 stdout 读取
    int stderr_fd_;  // 从进程的 stderr 读取
    int exit_code_;
    bool exited_;
    std::mutex mutex_;

    AIBinder* binder_ = nullptr;
    static AIBinder_Class* binderClass_;
};

// 客户端记录
struct ClientRecord {
    uid_t uid;
    pid_t pid;
    std::string packageName;
    int apiVersion;
    bool allowed;
    AIBinder* applicationBinder = nullptr;
};

/**
 * Shizuku 兼容服务
 *
 * 实现 IShizukuService 接口，让现有 Shizuku App 无缝迁移
 */
class ShizukuService {
public:
    static ShizukuService& getInstance();

    // 初始化并注册服务
    int init();

    // 启动服务线程
    void startThreadPool();

    // 停止服务
    void stop();

    // 获取调用者 UID
    uid_t getCallingUid();

    // 权限检查
    bool checkCallerPermission(uid_t uid);
    void allowUid(uid_t uid, bool allow);

private:
    ShizukuService() = default;
    ~ShizukuService();
    ShizukuService(const ShizukuService&) = delete;
    ShizukuService& operator=(const ShizukuService&) = delete;

    // Binder 事务处理
    static binder_status_t onTransact(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                      AParcel* out);

    // 各接口实现
    binder_status_t handleGetVersion(const AParcel* in, AParcel* out);
    binder_status_t handleGetUid(const AParcel* in, AParcel* out);
    binder_status_t handleCheckPermission(const AParcel* in, AParcel* out);
    binder_status_t handleNewProcess(const AParcel* in, AParcel* out);
    binder_status_t handleGetSELinuxContext(const AParcel* in, AParcel* out);
    binder_status_t handleGetSystemProperty(const AParcel* in, AParcel* out);
    binder_status_t handleSetSystemProperty(const AParcel* in, AParcel* out);
    binder_status_t handleCheckSelfPermission(const AParcel* in, AParcel* out);
    binder_status_t handleRequestPermission(const AParcel* in, AParcel* out);
    binder_status_t handleAttachApplication(const AParcel* in, AParcel* out);
    binder_status_t handleExit(const AParcel* in, AParcel* out);
    binder_status_t handleIsHidden(const AParcel* in, AParcel* out);
    binder_status_t handleGetFlagsForUid(const AParcel* in, AParcel* out);
    binder_status_t handleUpdateFlagsForUid(const AParcel* in, AParcel* out);

    // 辅助函数
    ClientRecord* findClient(uid_t uid, pid_t pid);
    ClientRecord* requireClient(uid_t uid, pid_t pid);
    RemoteProcessHolder* createProcess(const std::vector<std::string>& cmd,
                                       const std::vector<std::string>& env, const std::string& dir);

    AIBinder_Class* binderClass_ = nullptr;
    AIBinder* binder_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread serviceThread_;

    // 客户端管理
    std::mutex clientsMutex_;
    std::map<uint64_t, std::unique_ptr<ClientRecord>> clients_;  // key = (uid << 32) | pid

    // 权限管理 - 简化版，直接用 KSU allowlist
    std::mutex permMutex_;
    std::map<uid_t, bool> permissions_;
};

// 启动 Shizuku 兼容服务
void start_shizuku_service();

}  // namespace shizuku
}  // namespace ksud
