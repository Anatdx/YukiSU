// Constants and definitions
#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "uapi/supercall.h"
}

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

// Kasumi devices
constexpr const char* KASUMI_MIRROR_DEV = "/dev/kasumi_mirror";

}  // namespace hymo
