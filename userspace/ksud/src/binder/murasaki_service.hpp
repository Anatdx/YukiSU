// Murasaki Service - Binder Service Header
// KernelSU 内核级 API 服务端

#pragma once

#include <memory>
#include <string>

namespace ksud {
namespace murasaki {

// 权限等级
enum class PrivilegeLevel {
    SHELL = 0,  // Shizuku 兼容
    ROOT = 1,   // Sui 兼容
    KERNEL = 2  // Murasaki 内核级
};

/**
 * Murasaki Binder 服务
 *
 * 在 ksud 启动时注册到 ServiceManager
 * 提供给 App 直接调用内核功能的接口
 */
class MurasakiService {
public:
    static MurasakiService& getInstance();

    /**
     * 初始化并注册 Binder 服务
     * 应在 post-fs-data 阶段调用
     * @return 0 成功，负数错误码
     */
    int init();

    /**
     * 启动服务主循环
     * 会阻塞当前线程
     */
    void run();

    /**
     * 停止服务
     */
    void stop();

    /**
     * 检查服务是否运行中
     */
    bool isRunning() const;

    // ==================== 服务接口实现 ====================

    // 版本信息
    int getVersion();
    int getKernelSuVersion();
    PrivilegeLevel getPrivilegeLevel(int callingUid);
    bool isKernelModeAvailable();

    // SELinux 控制
    std::string getSelinuxContext(int pid);
    int setSelinuxContext(const std::string& context);

    // HymoFS 操作
    int hymoAddRule(const std::string& src, const std::string& target, int type);
    int hymoClearRules();
    int hymoSetStealth(bool enable);
    int hymoSetDebug(bool enable);
    int hymoSetMirrorPath(const std::string& path);
    int hymoFixMounts();
    std::string hymoGetActiveRules();

    // KSU 操作
    std::string getAppProfile(int uid);
    int setAppProfile(int uid, const std::string& profileJson);
    bool isUidGrantedRoot(int uid);
    bool shouldUmountForUid(int uid);
    int injectSepolicy(const std::string& rules);
    int addTryUmount(const std::string& path);
    int nukeExt4Sysfs();

private:
    MurasakiService() = default;
    ~MurasakiService();
    MurasakiService(const MurasakiService&) = delete;
    MurasakiService& operator=(const MurasakiService&) = delete;

    bool running_ = false;
    bool initialized_ = false;

    // IPC 实现 (定义在 murasaki_ipc.cpp)
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * 在后台线程启动 Murasaki 服务
 * 非阻塞调用
 */
void start_murasaki_service_async();

/**
 * 停止 Murasaki 服务
 */
void stop_murasaki_service();

/**
 * 检查 Murasaki 服务是否可用
 */
bool is_murasaki_service_available();

}  // namespace murasaki
}  // namespace ksud
