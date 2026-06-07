#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Kernel uapi headers — single source of truth for ioctl numbers,
// struct layouts, and feature IDs.  Guarded for userspace by
// #ifndef __KERNEL__ blocks in each uapi header.
extern "C" {
#include "uapi/supercall.h"
}

namespace ksud {

// C++ convenience aliases (keep callers unchanged)
using GetInfoCmd = ksu_get_info_cmd;
using ReportEventCmd = ksu_report_event_cmd;
using SetSepolicyCmd = ksu_set_sepolicy_cmd;
using CheckSafemodeCmd = ksu_check_safemode_cmd;
using GetFeatureCmd = ksu_get_feature_cmd;
using SetFeatureCmd = ksu_set_feature_cmd;
using GetWrapperFdCmd = ksu_get_wrapper_fd_cmd;
using GetSulogFdCmd = ksu_get_sulog_fd_cmd;
using ManageMarkCmd = ksu_manage_mark_cmd;
using NukeExt4SysfsCmd = ksu_nuke_ext4_sysfs_cmd;
using AddTryUmountCmd = ksu_add_try_umount_cmd;
using DynamicManagerSign = ksu_dynamic_manager_sign;
using DynamicManagerCmd = ksu_dynamic_manager_cmd;

// YukiSU-only: list umount ioctl (not in upstream uapi)
struct ListTryUmountCmd {
    uint64_t arg;
    uint32_t buf_size;
};
#define KSU_IOCTL_LIST_TRY_UMOUNT _IOC(_IOC_READ | _IOC_WRITE, 'K', 200, 0)

// API functions
int ksuctl(int request, void* arg);

int32_t get_version();
uint32_t get_flags();

int grant_root();
void report_post_fs_data();
void report_boot_complete();
void report_module_mounted();
bool check_kernel_safemode();

int set_sepolicy(const void* payload, uint64_t payload_len);

// Feature management
// Returns: pair<value, supported>
std::pair<uint64_t, bool> get_feature(uint32_t feature_id);
int set_feature(uint32_t feature_id, uint64_t value);

int get_wrapped_fd(int fd);
int get_sulog_fd();

// Mark management
uint32_t mark_get(int32_t pid);
int mark_set(int32_t pid);
int mark_unset(int32_t pid);
int mark_refresh();

int nuke_ext4_sysfs(const std::string& mnt);
int set_init_pgrp();
int set_dynamic_managers(const std::vector<DynamicManagerSign>& signs);

// Umount list management
int umount_list_wipe();
int umount_list_add(const std::string& path, uint32_t flags);
int umount_list_del(const std::string& path);
std::optional<std::string> umount_list_list();

}  // namespace ksud
