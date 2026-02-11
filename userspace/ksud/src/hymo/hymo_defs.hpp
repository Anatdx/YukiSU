// Constants and definitions
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hymo {

// Directories
constexpr const char* FALLBACK_CONTENT_DIR = "/data/adb/hymo/img_mnt/";
constexpr const char* BASE_DIR = "/data/adb/hymo/";
constexpr const char* RUN_DIR = "/data/adb/hymo/run/";
constexpr const char* STATE_FILE = "/data/adb/hymo/run/daemon_state.json";
constexpr const char* DAEMON_LOG_FILE = "/data/adb/hymo/daemon.log";
constexpr const char* SYSTEM_RW_DIR = "/data/adb/hymo/rw";

// Marker files
constexpr const char* DISABLE_FILE_NAME = "disable";
constexpr const char* REMOVE_FILE_NAME = "remove";
constexpr const char* SKIP_MOUNT_FILE_NAME = "skip_mount";
constexpr const char* REPLACE_DIR_FILE_NAME = ".replace";

// OverlayFS
constexpr const char* OVERLAY_SOURCE = "KSU";
constexpr const char* KSU_OVERLAY_SOURCE = OVERLAY_SOURCE;

// XAttr
constexpr const char* REPLACE_DIR_XATTR = "trusted.overlay.opaque";
constexpr const char* SELINUX_XATTR = "security.selinux";
constexpr const char* DEFAULT_SELINUX_CONTEXT = "u:object_r:system_file:s0";

// Standard Android partitions
const std::vector<std::string> BUILTIN_PARTITIONS = {"system",     "vendor", "product",
                                                     "system_ext", "odm",    "oem"};

// KSU IOCTLs
constexpr uint32_t KSU_INSTALL_MAGIC1 = 0xDEADBEEF;
constexpr uint32_t KSU_INSTALL_MAGIC2 = 0xCAFEBABE;
constexpr uint32_t KSU_IOCTL_NUKE_EXT4_SYSFS = 0x40004b11;
constexpr uint32_t KSU_IOCTL_ADD_TRY_UMOUNT = 0x40004b12;

// HymoFS Devices
constexpr const char* HYMO_MIRROR_DEV = "/dev/hymo_mirror";

}  // namespace hymo
