/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk - zygiskd <-> core wire protocol + multi-call entry.
 *
 * Author: Anatdx
 */
#pragma once

#include <cstdint>

namespace zygiskd {

/* core sends a one-byte Request (+ args); daemon replies with data and/or
 * passes fds via SCM_RIGHTS. */
enum class Request : uint8_t {
  GetProcessFlags = 1,  // arg u32 uid -> u32 StateFlag bits
  GetModuleCount = 2,   // -> u32 count
  GetModuleFd = 3,      // arg u32 index -> module lib fd
  ConnectCompanion = 4, // arg u32 index -> companion socket fd
  GetModuleDir = 5,     // arg u32 index -> module root dir fd
  GetConfig = 6, // -> struct yz_config (runtime config from yzconfig.json)
  GetStatus = 7, // -> u32 len + len bytes of status JSON (manager only;
                 //    SO_PEERCRED-gated to the authenticated manager uid)
};

/* abstract socket name (callers prepend the NUL); ABI-specific */
#if defined(__LP64__)
inline constexpr char kSocketName[] = "zygiskd64";
#else
inline constexpr char kSocketName[] = "zygiskd32";
#endif // #if defined(__LP64__)

} // namespace zygiskd

/* ksud dispatches here when invoked as `zygiskd` or `ksud zygiskd`. */
extern "C" int zygiskd_main(int argc, char **argv);
