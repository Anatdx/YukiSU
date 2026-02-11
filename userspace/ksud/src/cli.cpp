#include "cli.hpp"
#include "assets.hpp"
#include "boot/boot_patch.hpp"
#include "core/feature.hpp"
#include "core/hide_bootloader.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "debug.hpp"
#include "defs.hpp"
#include "flash/flash_partition.hpp"
#include "init_event.hpp"
#include "log.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "profile/profile.hpp"
#include "sepolicy/sepolicy.hpp"
#include "su.hpp"
#include "umount.hpp"
#include "utils.hpp"

#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace ksud {

void CliParser::add_option(const CliOption& opt) {
    options_.push_back(opt);
}

bool CliParser::parse(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg.empty())
            continue;

        // Check if it's an option
        if (arg[0] == '-') {
            bool found = false;
            std::string opt_name;
            std::string opt_value;

            // Long option
            if (arg.size() > 1 && arg[1] == '-') {
                std::string long_opt = arg.substr(2);
                size_t eq_pos = long_opt.find('=');
                if (eq_pos != std::string::npos) {
                    opt_name = long_opt.substr(0, eq_pos);
                    opt_value = long_opt.substr(eq_pos + 1);
                } else {
                    opt_name = long_opt;
                }

                for (const auto& opt : options_) {
                    if (opt.long_name == opt_name) {
                        found = true;
                        if (opt.takes_value && opt_value.empty() && i + 1 < argc) {
                            opt_value = argv[++i];
                        }
                        parsed_options_[opt_name] = opt_value.empty() ? "true" : opt_value;
                        break;
                    }
                }
            }
            // Short option
            else {
                char short_opt = arg[1];
                for (const auto& opt : options_) {
                    if (opt.short_name == short_opt) {
                        found = true;
                        opt_name = opt.long_name;
                        if (opt.takes_value && i + 1 < argc) {
                            opt_value = argv[++i];
                        }
                        parsed_options_[opt_name] = opt_value.empty() ? "true" : opt_value;
                        break;
                    }
                }
            }

            if (!found) {
                LOGE("Unknown option: %s", arg.c_str());
            }
        }
        // Positional argument
        else {
            if (subcommand_.empty()) {
                subcommand_ = arg;
            } else {
                positional_args_.push_back(arg);
            }
        }
    }

    return true;
}

std::optional<std::string> CliParser::get_option(const std::string& name) const {
    auto it = parsed_options_.find(name);
    if (it != parsed_options_.end()) {
        return it->second;
    }

    // Return default value if exists
    for (const auto& opt : options_) {
        if (opt.long_name == name && !opt.default_value.empty()) {
            return opt.default_value;
        }
    }

    return std::nullopt;
}

bool CliParser::has_option(const std::string& name) const {
    return parsed_options_.find(name) != parsed_options_.end();
}

static void print_usage() {
    printf("YukiSU userspace daemon\n\n");
    printf("USAGE: ksud <COMMAND>\n\n");
    printf("COMMANDS:\n");
    printf("  module         Manage KernelSU modules\n");
    printf("  post-fs-data   Trigger post-fs-data event\n");
    printf("  services       Trigger service event\n");
    printf("  boot-completed Trigger boot-complete event\n");
    printf("  install        Install KernelSU userspace\n");
    printf("  uninstall      Uninstall KernelSU\n");
    printf("  sepolicy       SELinux policy patch tool\n");
    printf("  profile        Manage app profiles\n");
    printf("  feature        Manage kernel features\n");
    printf("  boot-patch     Patch boot image\n");
    printf("  boot-restore   Restore boot image\n");
    printf("  boot-info      Show boot information\n");
    printf("  flash          Flash partition images\n");
    printf("  umount         Manage umount paths\n");
    printf("  kernel         Kernel interface\n");
    printf("  debug          For developers\n");
    printf("  help           Show this help\n");
    printf("  version        Show version\n");
}

static void print_version() {
    printf("ksud version %s (code: %s)\n", VERSION_NAME, VERSION_CODE);
}

// Module subcommand handlers
static int cmd_module(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: ksud module <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  install <ZIP>     Install module\n");
        printf("  uninstall <ID>    Uninstall module\n");
        printf("  enable <ID>       Enable module\n");
        printf("  disable <ID>      Disable module\n");
        printf("  action <ID>       Run module action\n");
        printf("  list              List all modules\n");
        printf("  config            Manage module config\n");
        return 1;
    }

    // Switch to init mount namespace
    if (!switch_mnt_ns(1)) {
        LOGE("Failed to switch mount namespace");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "install" && args.size() > 1) {
        return module_install(args[1]);
    } else if (subcmd == "uninstall" && args.size() > 1) {
        return module_uninstall(args[1]);
    } else if (subcmd == "undo-uninstall" && args.size() > 1) {
        return module_undo_uninstall(args[1]);
    } else if (subcmd == "enable" && args.size() > 1) {
        return module_enable(args[1]);
    } else if (subcmd == "disable" && args.size() > 1) {
        return module_disable(args[1]);
    } else if (subcmd == "action" && args.size() > 1) {
        return module_run_action(args[1]);
    } else if (subcmd == "list") {
        return module_list();
    } else if (subcmd == "config") {
        // Handle module config subcommands
        if (args.size() < 2) {
            printf("USAGE: ksud module config <get|set|list|delete|clear> ...\n");
            return 1;
        }
        return module_config_handle(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    printf("Unknown module subcommand: %s\n", subcmd.c_str());
    return 1;
}

// Feature subcommand handlers
static int cmd_feature(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: ksud feature <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  get <ID>        Get feature value\n");
        printf("  set <ID> <VAL>  Set feature value\n");
        printf("  list            List all features\n");
        printf("  check <ID>      Check feature status\n");
        printf("  load            Load config from file\n");
        printf("  save            Save config to file\n");
        printf("  hide-bl         Show bootloader hiding status\n");
        printf("  hide-bl enable  Enable bootloader hiding\n");
        printf("  hide-bl disable Disable bootloader hiding\n");
        printf("  hide-bl run     Run bootloader hiding now\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "get" && args.size() > 1) {
        return feature_get(args[1]);
    } else if (subcmd == "set" && args.size() > 2) {
        return feature_set(args[1], std::stoull(args[2]));
    } else if (subcmd == "list") {
        feature_list();
        return 0;
    } else if (subcmd == "check" && args.size() > 1) {
        return feature_check(args[1]);
    } else if (subcmd == "load") {
        return feature_load_config();
    } else if (subcmd == "save") {
        return feature_save_config();
    } else if (subcmd == "hide-bl") {
        // Bootloader hiding subcommand
        if (args.size() > 1) {
            const std::string& action = args[1];
            if (action == "enable") {
                set_bl_hiding_enabled(true);
                printf("Bootloader hiding enabled. Will take effect on next boot.\n");
                return 0;
            } else if (action == "disable") {
                set_bl_hiding_enabled(false);
                printf("Bootloader hiding disabled.\n");
                return 0;
            } else if (action == "run") {
                hide_bootloader_status();
                printf("Bootloader hiding executed.\n");
                return 0;
            }
        }
        // Show status
        bool enabled = is_bl_hiding_enabled();
        printf("Bootloader hiding: %s\n", enabled ? "enabled" : "disabled");
        return 0;
    }

    printf("Unknown feature subcommand: %s\n", subcmd.c_str());
    return 1;
}

// Debug subcommand handlers
static int cmd_debug(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: ksud debug <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  set-manager [PKG]  Set manager app\n");
        printf("  get-sign <APK>     Get APK signature\n");
        printf("  su [-g]            Root shell\n");
        printf("  version            Get kernel version\n");
        printf("  mark <get|mark|unmark|refresh> [PID]\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "set-manager") {
        std::string pkg = args.size() > 1 ? args[1] : "com.anatdx.yukisu";
        return debug_set_manager(pkg);
    } else if (subcmd == "get-sign" && args.size() > 1) {
        return debug_get_sign(args[1]);
    } else if (subcmd == "version") {
        printf("Kernel Version: %d\n", get_version());
        return 0;
    } else if (subcmd == "su") {
        bool global_mnt = args.size() > 1 && args[1] == "-g";
        return grant_root_shell(global_mnt);
    } else if (subcmd == "mark" && args.size() > 1) {
        return debug_mark(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    printf("Unknown debug subcommand: %s\n", subcmd.c_str());
    return 1;
}

// Umount subcommand handlers
static int cmd_umount(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: ksud umount <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  add <MNT> [-f FLAGS]  Add mount point\n");
        printf("  remove <MNT>          Remove mount point\n");
        printf("  list                  List all mount points\n");
        printf("  save                  Save config\n");
        printf("  apply                 Apply config\n");
        printf("  clear-custom          Clear custom paths\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "add" && args.size() > 1) {
        uint32_t flags = 0;
        if (args.size() > 3 && args[2] == "-f") {
            flags = std::stoul(args[3]);
        }
        return umount_list_add(args[1], flags) < 0 ? 1 : 0;
    } else if (subcmd == "remove" && args.size() > 1) {
        return umount_remove_entry(args[1]);
    } else if (subcmd == "list") {
        auto list = umount_list_list();
        if (list) {
            printf("%s", list->c_str());
        }
        return 0;
    } else if (subcmd == "save") {
        return umount_save_config();
    } else if (subcmd == "apply") {
        return umount_apply_config();
    } else if (subcmd == "clear-custom") {
        return umount_clear_config();
    }

    printf("Unknown umount subcommand: %s\n", subcmd.c_str());
    return 1;
}

// Kernel subcommand handlers
static int cmd_kernel(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: ksud kernel <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  nuke-ext4-sysfs <MNT>  Nuke ext4 sysfs\n");
        printf("  umount <add|del|wipe>  Manage umount list\n");
        printf("  notify-module-mounted  Notify module mounted\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "nuke-ext4-sysfs" && args.size() > 1) {
        return nuke_ext4_sysfs(args[1]);
    } else if (subcmd == "umount" && args.size() > 1) {
        const std::string& op = args[1];
        if (op == "add" && args.size() > 2) {
            uint32_t flags = args.size() > 3 ? std::stoul(args[3]) : 0;
            return umount_list_add(args[2], flags);
        } else if (op == "del" && args.size() > 2) {
            return umount_list_del(args[2]);
        } else if (op == "wipe") {
            return umount_list_wipe();
        }
    } else if (subcmd == "notify-module-mounted") {
        report_module_mounted();
        return 0;
    }

    printf("Unknown kernel subcommand: %s\n", subcmd.c_str());
    return 1;
}

// Sepolicy subcommand handlers
static int cmd_sepolicy(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: ksud sepolicy <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  patch <POLICY>   Patch sepolicy\n");
        printf("  apply <FILE>     Apply sepolicy from file\n");
        printf("  check <POLICY>   Check sepolicy\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "patch" && args.size() > 1) {
        return sepolicy_live_patch(args[1]);
    } else if (subcmd == "apply" && args.size() > 1) {
        return sepolicy_apply_file(args[1]);
    } else if (subcmd == "check" && args.size() > 1) {
        return sepolicy_check_rule(args[1]);
    }

    printf("Unknown sepolicy subcommand: %s\n", subcmd.c_str());
    return 1;
}

// Profile subcommand handlers
static int cmd_profile(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: ksud profile <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  get-sepolicy <PKG>       Get SELinux policy\n");
        printf("  set-sepolicy <PKG> <POL> Set SELinux policy\n");
        printf("  get-template <ID>        Get template\n");
        printf("  set-template <ID> <TPL>  Set template\n");
        printf("  delete-template <ID>     Delete template\n");
        printf("  list-templates           List templates\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "get-sepolicy" && args.size() > 1) {
        return profile_get_sepolicy(args[1]);
    } else if (subcmd == "set-sepolicy" && args.size() > 2) {
        return profile_set_sepolicy(args[1], args[2]);
    } else if (subcmd == "get-template" && args.size() > 1) {
        return profile_get_template(args[1]);
    } else if (subcmd == "set-template" && args.size() > 2) {
        return profile_set_template(args[1], args[2]);
    } else if (subcmd == "delete-template" && args.size() > 1) {
        return profile_delete_template(args[1]);
    } else if (subcmd == "list-templates") {
        return profile_list_templates();
    }

    printf("Unknown profile subcommand: %s\n", subcmd.c_str());
    return 1;
}

// Boot info subcommand handlers
static int cmd_boot_info(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: ksud boot-info <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  current-kmi         Show current KMI\n");
        printf("  supported-kmis      Show supported KMIs\n");
        printf("  is-ab-device        Check A/B device\n");
        printf("  default-partition   Show default partition\n");
        printf("  available-partitions List partitions\n");
        printf("  slot-suffix [-u]    Show slot suffix\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "current-kmi") {
        return boot_info_current_kmi();
    } else if (subcmd == "supported-kmis") {
        return boot_info_supported_kmis();
    } else if (subcmd == "is-ab-device") {
        return boot_info_is_ab_device();
    } else if (subcmd == "default-partition") {
        return boot_info_default_partition();
    } else if (subcmd == "available-partitions") {
        return boot_info_available_partitions();
    } else if (subcmd == "slot-suffix") {
        bool ota = args.size() > 1 && (args[1] == "-u" || args[1] == "--ota");
        return boot_info_slot_suffix(ota);
    }

    printf("Unknown boot-info subcommand: %s\n", subcmd.c_str());
    return 1;
}

// Flash subcommand handlers
static int cmd_flash_new(const std::vector<std::string>& args) {
    using namespace flash;

    if (args.empty()) {
        printf("USAGE: ksud flash <SUBCOMMAND> [OPTIONS]\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  image <IMAGE> <PARTITION>  Flash image to partition\n");
        printf("  backup <PARTITION> <OUT>   Backup partition to file\n");
        printf("  list [--slot SLOT] [--all] List available partitions\n");
        printf("  info <PARTITION>           Show partition info\n");
        printf("  slots                      Show slot information (A/B devices)\n");
        printf("  map <SLOT>                 Map logical partitions for inactive slot\n");
        printf("  avb                        Show AVB/dm-verity status\\n");
        printf("  avb disable                Disable AVB/dm-verity\\n");
        printf("  kernel [--slot SLOT]       Show kernel version\\n");
        printf("  boot-info                  Show boot slot information\\n");
        printf("\nOPTIONS:\n");
        printf("  --slot <a|b|_a|_b>         Target specific slot (for A/B devices)\n");
        printf("                             Default: current active slot\n");
        printf("  --all                      List all partitions (not just common ones)\n");
        printf("\nEXAMPLES:\n");
        printf("  ksud flash image boot.img boot\n");
        printf("  ksud flash image boot.img boot --slot _b\n");
        printf("  ksud flash backup boot /sdcard/boot-backup.img --slot _a\n");
        printf("  ksud flash list\n");
        printf("  ksud flash list --all\n");
        printf("  ksud flash slots\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    // Parse common options
    std::string target_slot;
    bool scan_all = false;
    std::vector<std::string> filtered_args;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--slot" && i + 1 < args.size()) {
            target_slot = args[++i];
            // Normalize slot format (_a, a -> _a)
            if (!target_slot.empty() && target_slot[0] != '_') {
                target_slot = "_" + target_slot;
            }
        } else if (args[i] == "--all") {
            scan_all = true;
        } else {
            filtered_args.push_back(args[i]);
        }
    }

    if (filtered_args[0] == "image" && filtered_args.size() >= 3) {
        std::string image_path = filtered_args[1];
        std::string partition = filtered_args[2];

        printf("Flashing %s to %s", image_path.c_str(), partition.c_str());
        if (!target_slot.empty()) {
            printf(" (slot: %s)", target_slot.c_str());
        }
        printf("...\n");

        if (ksud::flash::flash_partition(image_path, partition, target_slot)) {
            printf("Flash successful!\n");
            return 0;
        } else {
            printf("Flash failed!\n");
            return 1;
        }

    } else if (filtered_args[0] == "backup" && filtered_args.size() >= 3) {
        std::string partition = filtered_args[1];
        std::string output = filtered_args[2];

        printf("Backing up %s to %s", partition.c_str(), output.c_str());
        if (!target_slot.empty()) {
            printf(" (slot: %s)", target_slot.c_str());
        }
        printf("...\n");

        if (ksud::flash::backup_partition(partition, output, target_slot)) {
            printf("Backup successful!\n");
            return 0;
        } else {
            printf("Backup failed!\n");
            return 1;
        }

    } else if (filtered_args[0] == "list") {
        std::string slot =
            target_slot.empty() ? ksud::flash::get_current_slot_suffix() : target_slot;
        auto partitions = ksud::flash::get_available_partitions(scan_all);

        if (scan_all) {
            printf("All partitions");
        } else {
            printf("Common partitions");
        }
        if (ksud::flash::is_ab_device() && !slot.empty()) {
            printf(" (slot: %s)", slot.c_str());
        }
        printf(":\n");

        for (const auto& p : partitions) {
            auto info = ksud::flash::get_partition_info(p, slot);
            const char* type = info.is_logical ? "logical" : "physical";
            const char* marker = "";
            if (ksud::flash::is_dangerous_partition(p)) {
                marker = " [DANGEROUS]";
            }
            printf("  %-20s [%s, %lu bytes]%s\n", p.c_str(), type, (unsigned long)info.size,
                   marker);
        }
        return 0;

    } else if (filtered_args[0] == "info" && filtered_args.size() >= 2) {
        std::string partition = filtered_args[1];
        std::string slot =
            target_slot.empty() ? ksud::flash::get_current_slot_suffix() : target_slot;
        auto info = ksud::flash::get_partition_info(partition, slot);

        if (!info.exists) {
            printf("Partition %s not found\n", partition.c_str());
            return 1;
        }

        printf("Partition: %s\n", info.name.c_str());
        printf("Block device: %s\n", info.block_device.c_str());
        printf("Type: %s\n", info.is_logical ? "logical" : "physical");
        printf("Size: %lu bytes (%.2f MB)\n", (unsigned long)info.size,
               info.size / 1024.0 / 1024.0);

        if (ksud::flash::is_ab_device()) {
            printf("Slot: %s\n", slot.c_str());
        }
        return 0;

    } else if (filtered_args[0] == "slots") {
        // Show slot information for A/B devices
        if (!ksud::flash::is_ab_device()) {
            printf("This device is not A/B partitioned\n");
            return 0;
        }

        std::string current_slot = ksud::flash::get_current_slot_suffix();
        std::string other_slot = (current_slot == "_a") ? "_b" : "_a";

        printf("Slot Information:\n");
        printf("  Current slot: %s\n", current_slot.c_str());
        printf("  Other slot:   %s\n", other_slot.c_str());

        // Try to get bootctl info if available
        auto result = exec_command({"getprop", "ro.boot.slot_suffix"});
        if (result.exit_code == 0) {
            printf("  Property ro.boot.slot_suffix: %s\n", trim(result.stdout_str).c_str());
        }

        return 0;

    } else if (filtered_args[0] == "map" && filtered_args.size() >= 2) {
        std::string slot = filtered_args[1];
        // Normalize slot format
        if (!slot.empty() && slot[0] != '_') {
            slot = "_" + slot;
        }

        printf("Mapping logical partitions for slot %s...\n", slot.c_str());
        if (ksud::flash::map_logical_partitions(slot)) {
            printf("Mapping successful!\n");
            printf("You can now use 'ksud flash list --slot %s --all' to see mapped partitions\n",
                   slot.c_str());
            return 0;
        } else {
            printf("Mapping failed or no partitions to map\n");
            return 1;
        }

    } else if (filtered_args[0] == "avb") {
        if (filtered_args.size() >= 2 && filtered_args[1] == "disable") {
            printf("Disabling AVB/dm-verity...\n");
            if (ksud::flash::patch_vbmeta_disable_verification()) {
                printf("AVB/dm-verity disabled successfully!\n");
                printf("Reboot required for changes to take effect.\n");
                return 0;
            } else {
                printf("Failed to disable AVB/dm-verity\n");
                return 1;
            }
        } else {
            std::string status = ksud::flash::get_avb_status();
            if (status.empty()) {
                printf("Failed to get AVB status\n");
                return 1;
            }
            printf("AVB/dm-verity status: %s\n", status.c_str());
            return 0;
        }

    } else if (filtered_args[0] == "kernel") {
        std::string version = ksud::flash::get_kernel_version(target_slot);
        if (version.empty()) {
            printf("Failed to get kernel version\n");
            return 1;
        }
        printf("Kernel version: %s\n", version.c_str());
        return 0;

    } else if (filtered_args[0] == "boot-info") {
        std::string info = ksud::flash::get_boot_slot_info();
        printf("%s\n", info.c_str());
        return 0;
    }

    printf("Unknown flash subcommand: %s\n", subcmd.c_str());
    printf("Run 'ksud flash' for usage\n");
    return 1;
}

int cli_run(int argc, char* argv[]) {
    // Initialize logging
    log_init("KernelSU");

    // Check if invoked as su or sh
    std::string arg0 = argv[0];
    size_t last_slash = arg0.rfind('/');
    std::string basename = (last_slash != std::string::npos) ? arg0.substr(last_slash + 1) : arg0;

    if (basename == "su") {
        return su_main(argc, argv);
    }

    // If invoked as "sh", forward to busybox sh with all arguments
    // This handles the case where /system/bin/sh is a hardlink to ksud
    if (basename == "sh") {
        // Use busybox to handle shell operations
        const char* busybox = "/data/adb/ksu/bin/busybox";

        // Build argv for busybox: busybox sh [original args...]
        std::vector<char*> new_argv;
        new_argv.push_back(const_cast<char*>("sh"));
        for (int i = 1; i < argc; i++) {
            new_argv.push_back(argv[i]);
        }
        new_argv.push_back(nullptr);

        // Set ASH_STANDALONE to make busybox ash work properly
        setenv("ASH_STANDALONE", "1", 1);

        execv(busybox, new_argv.data());
        // If busybox fails, try system sh as fallback
        execv("/system/bin/toybox", new_argv.data());
        _exit(127);
    }

    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string cmd = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; i++) {
        args.push_back(argv[i]);
    }

    LOGI("command: %s", cmd.c_str());

    // Dispatch commands
    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        print_usage();
        return 0;
    } else if (cmd == "version" || cmd == "-v" || cmd == "--version") {
        print_version();
        return 0;
    } else if (cmd == "post-fs-data") {
        return on_post_data_fs();
    } else if (cmd == "services") {
        on_services();
        return 0;
    } else if (cmd == "boot-completed") {
        on_boot_completed();
        return 0;
    } else if (cmd == "module") {
        return cmd_module(args);
    } else if (cmd == "install") {
        std::optional<std::string> magiskboot;
        for (size_t i = 0; i < args.size(); i++) {
            if (args[i] == "--magiskboot" && i + 1 < args.size()) {
                magiskboot = args[i + 1];
            }
        }
        return install(magiskboot);
    } else if (cmd == "uninstall") {
        std::optional<std::string> magiskboot;
        for (size_t i = 0; i < args.size(); i++) {
            if (args[i] == "--magiskboot" && i + 1 < args.size()) {
                magiskboot = args[i + 1];
            }
        }
        return uninstall(magiskboot);
    } else if (cmd == "sepolicy") {
        return cmd_sepolicy(args);
    } else if (cmd == "profile") {
        return cmd_profile(args);
    } else if (cmd == "feature") {
        return cmd_feature(args);
    } else if (cmd == "boot-patch") {
        return boot_patch(args);
    } else if (cmd == "boot-restore") {
        return boot_restore(args);
    } else if (cmd == "boot-info") {
        return cmd_boot_info(args);
    } else if (cmd == "umount") {
        return cmd_umount(args);
    } else if (cmd == "kernel") {
        return cmd_kernel(args);
    } else if (cmd == "debug") {
        return cmd_debug(args);
    } else if (cmd == "flash") {
        return cmd_flash_new(args);
    }

    printf("Unknown command: %s\n", cmd.c_str());
    print_usage();
    return 1;
}

}  // namespace ksud
