// Murasaki IPC Server - Unix Socket Implementation
// 在等待真正的 Binder 实现之前的过渡方案

#include "../core/ksucalls.hpp"
#include "../hymo/mount/hymofs.hpp"
#include "../log.hpp"
#include "murasaki_protocol.hpp"
#include "murasaki_service.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

namespace ksud {
namespace murasaki {

// Socket 路径
static constexpr const char* SOCKET_PATH = "\0murasaki";  // Abstract socket

class MurasakiService::Impl {
public:
    int server_fd = -1;
    std::vector<std::thread> client_threads;

    int start_server() {
        // 创建 Unix socket
        server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd < 0) {
            LOGE("Failed to create socket: %s", strerror(errno));
            return -errno;
        }

        // 绑定到抽象命名空间
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        // 使用抽象命名空间 (第一个字节为 0)
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        socklen_t len = offsetof(struct sockaddr_un, sun_path) + strlen(SOCKET_PATH);

        if (bind(server_fd, (struct sockaddr*)&addr, len) < 0) {
            LOGE("Failed to bind socket: %s", strerror(errno));
            close(server_fd);
            server_fd = -1;
            return -errno;
        }

        // 监听
        if (listen(server_fd, 10) < 0) {
            LOGE("Failed to listen: %s", strerror(errno));
            close(server_fd);
            server_fd = -1;
            return -errno;
        }

        LOGI("Murasaki IPC server started on abstract socket");
        return 0;
    }

    void stop_server() {
        if (server_fd >= 0) {
            close(server_fd);
            server_fd = -1;
        }
    }

    void handle_client(int client_fd, uid_t client_uid) {
        LOGI("Client connected: fd=%d, uid=%d", client_fd, client_uid);

        while (true) {
            // 读取请求头
            RequestHeader req_header;
            ssize_t n = recv(client_fd, &req_header, sizeof(req_header), MSG_WAITALL);
            if (n <= 0) {
                if (n < 0) {
                    LOGW("recv header failed: %s", strerror(errno));
                }
                break;
            }

            if (!req_header.is_valid()) {
                LOGW("Invalid request header");
                break;
            }

            // 读取请求数据
            std::vector<uint8_t> req_data(req_header.data_size);
            if (req_header.data_size > 0) {
                n = recv(client_fd, req_data.data(), req_header.data_size, MSG_WAITALL);
                if (n != (ssize_t)req_header.data_size) {
                    LOGW("recv data failed");
                    break;
                }
            }

            // 处理请求
            std::vector<uint8_t> resp_data;
            int result = process_command(static_cast<Command>(req_header.cmd), client_uid, req_data,
                                         resp_data);

            // 发送响应
            ResponseHeader resp_header;
            resp_header.init(req_header.seq, result, resp_data.size());

            send(client_fd, &resp_header, sizeof(resp_header), 0);
            if (!resp_data.empty()) {
                send(client_fd, resp_data.data(), resp_data.size(), 0);
            }
        }

        close(client_fd);
        LOGI("Client disconnected: uid=%d", client_uid);
    }

    int process_command(Command cmd, uid_t caller_uid, const std::vector<uint8_t>& req_data,
                        std::vector<uint8_t>& resp_data) {
        auto& service = MurasakiService::getInstance();

        switch (cmd) {
        case Command::GET_VERSION: {
            int32_t ver = service.getVersion();
            resp_data.resize(sizeof(ver));
            memcpy(resp_data.data(), &ver, sizeof(ver));
            return 0;
        }

        case Command::GET_KSU_VERSION: {
            int32_t ver = service.getKernelSuVersion();
            resp_data.resize(sizeof(ver));
            memcpy(resp_data.data(), &ver, sizeof(ver));
            return 0;
        }

        case Command::GET_PRIVILEGE_LEVEL: {
            auto level = service.getPrivilegeLevel(caller_uid);
            int32_t lv = static_cast<int32_t>(level);
            resp_data.resize(sizeof(lv));
            memcpy(resp_data.data(), &lv, sizeof(lv));
            return 0;
        }

        case Command::IS_KERNEL_MODE_AVAILABLE: {
            BoolResponse resp;
            resp.value = service.isKernelModeAvailable() ? 1 : 0;
            resp_data.resize(sizeof(resp));
            memcpy(resp_data.data(), &resp, sizeof(resp));
            return 0;
        }

        case Command::GET_SELINUX_CONTEXT: {
            int pid = 0;
            if (req_data.size() >= sizeof(SelinuxContextRequest)) {
                auto* req = reinterpret_cast<const SelinuxContextRequest*>(req_data.data());
                pid = req->pid;
            }
            std::string ctx = service.getSelinuxContext(pid);
            resp_data.resize(ctx.size() + 1);
            memcpy(resp_data.data(), ctx.c_str(), ctx.size() + 1);
            return 0;
        }

        case Command::HYMO_ADD_RULE: {
            if (req_data.size() < sizeof(HymoAddRuleRequest)) {
                return -EINVAL;
            }
            auto* req = reinterpret_cast<const HymoAddRuleRequest*>(req_data.data());
            return service.hymoAddRule(req->src, req->target, req->type);
        }

        case Command::HYMO_CLEAR_RULES: {
            return service.hymoClearRules();
        }

        case Command::HYMO_SET_STEALTH: {
            if (req_data.size() < sizeof(HymoSetBoolRequest)) {
                return -EINVAL;
            }
            auto* req = reinterpret_cast<const HymoSetBoolRequest*>(req_data.data());
            return service.hymoSetStealth(req->value != 0);
        }

        case Command::HYMO_SET_DEBUG: {
            if (req_data.size() < sizeof(HymoSetBoolRequest)) {
                return -EINVAL;
            }
            auto* req = reinterpret_cast<const HymoSetBoolRequest*>(req_data.data());
            return service.hymoSetDebug(req->value != 0);
        }

        case Command::HYMO_SET_MIRROR_PATH: {
            if (req_data.size() < sizeof(HymoSetPathRequest)) {
                return -EINVAL;
            }
            auto* req = reinterpret_cast<const HymoSetPathRequest*>(req_data.data());
            return service.hymoSetMirrorPath(req->path);
        }

        case Command::HYMO_FIX_MOUNTS: {
            return service.hymoFixMounts();
        }

        case Command::HYMO_GET_ACTIVE_RULES: {
            std::string rules = service.hymoGetActiveRules();
            resp_data.resize(rules.size() + 1);
            memcpy(resp_data.data(), rules.c_str(), rules.size() + 1);
            return 0;
        }

        case Command::IS_UID_GRANTED_ROOT: {
            if (req_data.size() < sizeof(UidRequest)) {
                return -EINVAL;
            }
            auto* req = reinterpret_cast<const UidRequest*>(req_data.data());
            BoolResponse resp;
            resp.value = service.isUidGrantedRoot(req->uid) ? 1 : 0;
            resp_data.resize(sizeof(resp));
            memcpy(resp_data.data(), &resp, sizeof(resp));
            return 0;
        }

        case Command::SHOULD_UMOUNT_FOR_UID: {
            if (req_data.size() < sizeof(UidRequest)) {
                return -EINVAL;
            }
            auto* req = reinterpret_cast<const UidRequest*>(req_data.data());
            BoolResponse resp;
            resp.value = service.shouldUmountForUid(req->uid) ? 1 : 0;
            resp_data.resize(sizeof(resp));
            memcpy(resp_data.data(), &resp, sizeof(resp));
            return 0;
        }

        case Command::INJECT_SEPOLICY: {
            if (req_data.size() < sizeof(SepolicyRequest)) {
                return -EINVAL;
            }
            auto* req = reinterpret_cast<const SepolicyRequest*>(req_data.data());
            return service.injectSepolicy(req->rules);
        }

        case Command::NUKE_EXT4_SYSFS: {
            return service.nukeExt4Sysfs();
        }

        default:
            LOGW("Unknown command: %u", static_cast<uint32_t>(cmd));
            return -ENOSYS;
        }
    }
};

// MurasakiService 析构函数 - 需要在 Impl 定义之后
MurasakiService::~MurasakiService() = default;

// 更新 MurasakiService 的 run 方法以使用 Impl
void MurasakiService::run() {
    if (!initialized_) {
        LOGE("MurasakiService: Not initialized!");
        return;
    }

    impl_ = std::make_unique<Impl>();

    if (impl_->start_server() != 0) {
        LOGE("MurasakiService: Failed to start IPC server");
        return;
    }

    running_ = true;
    LOGI("MurasakiService: Accepting connections...");

    while (running_) {
        struct pollfd pfd = {impl_->server_fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 1000);  // 1秒超时

        if (ret < 0) {
            if (errno != EINTR) {
                LOGE("poll failed: %s", strerror(errno));
                break;
            }
            continue;
        }

        if (ret == 0)
            continue;  // 超时

        if (pfd.revents & POLLIN) {
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_fd = accept(impl_->server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                LOGW("accept failed: %s", strerror(errno));
                continue;
            }

            // 获取客户端 UID
            struct ucred cred;
            socklen_t cred_len = sizeof(cred);
            uid_t client_uid = 0;
            if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0) {
                client_uid = cred.uid;
            }

            // 在新线程中处理客户端
            std::thread client_thread(
                [this, client_fd, client_uid]() { impl_->handle_client(client_fd, client_uid); });
            client_thread.detach();
        }
    }

    impl_->stop_server();
    LOGI("MurasakiService: Stopped");
}

}  // namespace murasaki
}  // namespace ksud
