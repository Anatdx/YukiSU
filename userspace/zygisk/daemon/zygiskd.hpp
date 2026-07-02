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
  RevertMount =
      8, // (denylist_mode==2) revert caller's module mounts: zygiskd
         //   resolves caller pid via SO_PEERCRED, drives kernel umount
  SelfDestruct =
      9,    // (denylist_mode==1) core unhooked; umount + munmap its segs
  Log = 10, // u16 len + len bytes -> daemon writes them to /dev/kmsg (dmesg)
  PatchText =
      11, // u64 addr + u32 len + len bytes -> kernel writes them into the
          //   caller's mm via access_process_vm(FOLL_FORCE) (no mprotect, no
          //   VMA split); used by the specialize inline-hook to patch code
  ReportZygote =
      12, // core -> daemon, no args: daemon records peer pid + zygote socket
          //   name parsed from /proc/<pid>/cmdline for manager status
  GetNativeModuleCount = 13,    // -> u32 count
  GetNativeModuleInfo = 14,     // arg u32 index -> NativeModuleInfo
  GetNativeModuleFd = 15,       // arg u32 index -> module lib fd
  ConnectNativeCompanion = 16,  // arg u32 index -> companion socket fd
  RestoreNativeLoadPolicy = 17, // restore temporary native load file allow
  ReportNativeInjection = 18,   // arg u32 index -> record native load success
};

inline constexpr uint32_t kNativeModuleNameMax = 64;
inline constexpr uint32_t kNativeModuleTargetMax = 256;
inline constexpr uint32_t kNativeModulePathMax = 512;

struct NativeModuleInfo {
  uint8_t target_type;
  uint8_t has_companion;
  uint16_t reserved;
  char module_id[kNativeModuleNameMax];
  char target[kNativeModuleTargetMax];
  char lib_path[kNativeModulePathMax];
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
