#include "magisk_compat/su_protocol.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using namespace ksud::sucompat;

int connect_msud() {
  const int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    return -1;
  }

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  const size_t namelen = strlen(kSuSocketName);
  memcpy(addr.sun_path + 1, kSuSocketName, namelen);
  const socklen_t addrlen = static_cast<socklen_t>(
      offsetof(struct sockaddr_un, sun_path) + 1 + namelen);

  if (connect(sock, reinterpret_cast<struct sockaddr *>(&addr), addrlen) != 0) {
    close(sock);
    return -1;
  }
  return sock;
}

} // namespace

int main(int argc, char **argv, char **envp) {
  const int sock = connect_msud();
  if (sock < 0) {
    (void)fprintf(stderr, "su: authorization daemon unavailable\n");
    return 1;
  }

  std::array<char, 4096> cwdbuf{};
  const char *cwd = getcwd(cwdbuf.data(), cwdbuf.size());

  uint32_t na = 0;
  uint32_t ne = 0;
  const std::string payload =
      build_payload(cwd ? cwd : "/", argc, argv, envp, &na, &ne);
  if (payload.size() > kSuMaxPayload) {
    (void)fprintf(stderr, "su: request too large\n");
    return 1;
  }

  SuRequest hdr{};
  hdr.magic = kSuMagic;
  hdr.argc = na;
  hdr.envc = ne;
  hdr.payload_len = static_cast<uint32_t>(payload.size());

  const std::array<int, 3> fds = {0, 1, 2};
  if (!send_with_fds(sock, &hdr, sizeof(hdr), fds.data(),
                     static_cast<int>(fds.size())) ||
      !write_all(sock, payload.data(), payload.size())) {
    (void)fprintf(stderr, "su: failed to send request\n");
    return 1;
  }

  SuResult res{};
  if (!read_all(sock, &res, sizeof(res)) || res.magic != kSuMagic) {
    (void)fprintf(stderr, "su: no response from authorization daemon\n");
    return 1;
  }
  if (res.granted == 0) {
    (void)fprintf(stderr, "su: access denied\n");
    return 1;
  }
  return res.exit_code;
}
