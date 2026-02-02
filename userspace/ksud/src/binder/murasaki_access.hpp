// Murasaki access control helpers
// Shared by MurasakiBinderService and ShizukuService
//
// Goal:
// - Manager app (kernel-verified) always has highest privilege.
// - Manager can grant/revoke Murasaki/Shizuku access for other UIDs.
// - Access list is userspace-owned (file-based), independent from KSU su allowlist.
//
#pragma once

#include <sys/types.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace ksud::access {

// Kernel-reported manager UID (full UID). Returns nullopt if unknown/unset.
std::optional<uid_t> get_manager_uid();

// True if uid equals kernel-reported manager UID.
bool is_manager_uid(uid_t uid);

// Murasaki/Shizuku client allowlist management (UID-based)
bool is_murasaki_allowed(uid_t uid);
bool grant_murasaki(uid_t uid);
bool revoke_murasaki(uid_t uid);
std::vector<int32_t> list_murasaki_uids();

}  // namespace ksud::access

