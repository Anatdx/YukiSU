#pragma once

#include <cstdint>
#include <string>

namespace ksud {

// Version info
constexpr const char* KSUD_VERSION = "1.0.0";
constexpr int KSUD_VERSION_CODE = 10000;
extern const char* VERSION_CODE;
extern const char* VERSION_NAME;

// Paths
constexpr const char* ADB_DIR = "/data/adb/";
constexpr const char* WORKING_DIR = "/data/adb/ksu/";
constexpr const char* BINARY_DIR = "/data/adb/ksu/bin/";
constexpr const char* LOG_DIR = "/data/adb/ksu/log/";

// Binary tool paths
constexpr const char* BUSYBOX_PATH = "/data/adb/ksu/bin/busybox";
constexpr const char* RESETPROP_PATH = "/data/adb/ksu/bin/resetprop";
constexpr const char* BOOTCTL_PATH = "/data/adb/ksu/bin/bootctl";

constexpr const char* PROFILE_DIR = "/data/adb/ksu/profile/";
constexpr const char* PROFILE_SELINUX_DIR = "/data/adb/ksu/profile/selinux/";
constexpr const char* PROFILE_TEMPLATE_DIR = "/data/adb/ksu/profile/templates/";

constexpr const char* KSURC_PATH = "/data/adb/ksu/.ksurc";
constexpr const char* DAEMON_PATH = "/data/adb/ksud";
constexpr const char* MAGISKBOOT_PATH = "/data/adb/ksu/bin/magiskboot";
constexpr const char* DAEMON_LINK_PATH = "/data/adb/ksu/bin/ksud";

constexpr const char* MODULE_DIR = "/data/adb/modules/";
constexpr const char* MODULE_UPDATE_DIR = "/data/adb/modules_update/";
constexpr const char* METAMODULE_DIR = "/data/adb/metamodule/";

constexpr const char* MODULE_WEB_DIR = "webroot";
constexpr const char* MODULE_ACTION_SH = "action.sh";
constexpr const char* DISABLE_FILE_NAME = "disable";
constexpr const char* UPDATE_FILE_NAME = "update";
constexpr const char* REMOVE_FILE_NAME = "remove";

// Module config system
constexpr const char* MODULE_CONFIG_DIR = "/data/adb/ksu/module_configs/";
constexpr const char* PERSIST_CONFIG_NAME = "persist.config";
constexpr const char* TEMP_CONFIG_NAME = "tmp.config";

// Metamodule support
constexpr const char* METAMODULE_MOUNT_SCRIPT = "metamount.sh";
constexpr const char* METAMODULE_METAINSTALL_SCRIPT = "metainstall.sh";
constexpr const char* METAMODULE_METAUNINSTALL_SCRIPT = "metauninstall.sh";

// Backup
constexpr const char* KSU_BACKUP_DIR = "/data/adb/ksu/";
constexpr const char* KSU_BACKUP_FILE_PREFIX = "ksu_backup_";
constexpr const char* BACKUP_FILENAME = "stock_image.sha1";
constexpr const char* UMOUNT_CONFIG_PATH = "/data/adb/ksu/.umount";

// Feature IDs - must match kernel definitions
enum class FeatureId : uint32_t {
    SuCompat = 0,
    KernelUmount = 1,
    EnhancedSecurity = 2,
    SuLog = 100,
};

// ioctl constants
constexpr uint32_t KSUD_MAGIC = 'K';

// Event constants
constexpr uint32_t EVENT_POST_FS_DATA = 1;
constexpr uint32_t EVENT_BOOT_COMPLETED = 2;
constexpr uint32_t EVENT_MODULE_MOUNTED = 3;

// Mark operation constants
constexpr uint32_t KSU_MARK_GET = 1;
constexpr uint32_t KSU_MARK_MARK = 2;
constexpr uint32_t KSU_MARK_UNMARK = 3;
constexpr uint32_t KSU_MARK_REFRESH = 4;

// Umount operation constants
constexpr uint8_t UMOUNT_WIPE = 0;
constexpr uint8_t UMOUNT_ADD = 1;
constexpr uint8_t UMOUNT_DEL = 2;

}  // namespace ksud
