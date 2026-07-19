#include "magisk_compat/su_protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace ksud::sucompat {

std::string build_payload(const std::string& cwd, int argc, char** argv, char** envp,
                          uint32_t* out_argc, uint32_t* out_envc) {
    std::string buf;
    buf.append(cwd);
    buf.push_back('\0');

    uint32_t na = 0;
    for (int i = 0; i < argc && argv && argv[i]; ++i) {
        buf.append(argv[i]);
        buf.push_back('\0');
        ++na;
    }

    uint32_t ne = 0;
    for (int i = 0; envp && envp[i]; ++i) {
        buf.append(envp[i]);
        buf.push_back('\0');
        ++ne;
    }

    *out_argc = na;
    *out_envc = ne;
    return buf;
}

bool parse_payload(const std::string& buf, uint32_t argc, uint32_t envc, std::string* cwd,
                   std::vector<std::string>* args, std::vector<std::string>* env) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start < buf.size()) {
        const size_t nul = buf.find('\0', start);
        if (nul == std::string::npos) {
            return false;
        }
        fields.emplace_back(buf, start, nul - start);
        start = nul + 1;
    }

    if (fields.size() != static_cast<size_t>(1) + argc + envc) {
        return false;
    }

    *cwd = fields[0];
    args->assign(fields.begin() + 1, fields.begin() + 1 + argc);
    env->assign(fields.begin() + 1 + argc, fields.end());
    return true;
}

bool write_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

bool read_all(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = read(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

bool send_with_fds(int sock, const void* buf, size_t len, const int* fds, int nfds) {
    struct iovec iov{};
    iov.iov_base = const_cast<void*>(buf);
    iov.iov_len = len;

    char cmsg[CMSG_SPACE(sizeof(int) * 3)];
    memset(cmsg, 0, sizeof(cmsg));

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (nfds > 0) {
        nfds = std::min(nfds, 3);
        msg.msg_control = cmsg;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr* h = CMSG_FIRSTHDR(&msg);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(h), fds, sizeof(int) * nfds);
    }

    for (;;) {
        const ssize_t n = sendmsg(sock, &msg, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        return static_cast<size_t>(n) == len;
    }
}

bool recv_with_fds(int sock, void* buf, size_t len, int* out_fds, int max_fds, int* out_nfds) {
    *out_nfds = 0;

    struct iovec iov{};
    iov.iov_base = buf;
    iov.iov_len = len;

    char cmsg[CMSG_SPACE(sizeof(int) * 3)];
    memset(cmsg, 0, sizeof(cmsg));

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg;
    msg.msg_controllen = sizeof(cmsg);

    ssize_t n;
    for (;;) {
        n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (n != static_cast<ssize_t>(len)) {
        return false;
    }

    for (struct cmsghdr* h = CMSG_FIRSTHDR(&msg); h != nullptr; h = CMSG_NXTHDR(&msg, h)) {
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
            const int payload =
                static_cast<int>(h->cmsg_len - CMSG_LEN(0)) / static_cast<int>(sizeof(int));
            const int* data = reinterpret_cast<int*>(CMSG_DATA(h));
            for (int i = 0; i < payload; ++i) {
                if (*out_nfds < max_fds) {
                    out_fds[(*out_nfds)++] = data[i];
                } else {
                    close(data[i]);
                }
            }
        }
    }
    return true;
}

}  // namespace ksud::sucompat
