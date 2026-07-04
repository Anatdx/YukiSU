/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk daemon protocol.
 *
 * Author: Anatdx
 */
#pragma once

#include <cstdint>

namespace zygiskd {

/* One-byte request protocol. */
enum class Request : uint8_t {
  GetProcessFlags = 1,
  GetModuleCount = 2,
  GetModuleFd = 3,
  ConnectCompanion = 4,
  GetModuleDir = 5,
  GetConfig = 6, // -> struct yz_config (runtime config from yzconfig.json)
  GetStatus = 7,
  RevertMount = 8,
  SelfDestruct = 9,
  Log = 10,
  PatchText = 11,
  ReportZygote = 12,
  GetNativeModuleCount = 13,
  GetNativeModuleInfo = 14,
  GetNativeModuleFd = 15,
  ConnectNativeCompanion = 16,
  RestoreNativeLoadPolicy = 17,
  ReportNativeInjection = 18,
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

#if defined(__LP64__)
inline constexpr char kSocketName[] = "zygiskd64";
#else
inline constexpr char kSocketName[] = "zygiskd32";
#endif // #if defined(__LP64__)

} // namespace zygiskd

extern "C" int zygiskd_main(int argc, char **argv);
