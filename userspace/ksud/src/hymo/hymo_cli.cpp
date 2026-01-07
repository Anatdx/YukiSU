// hymo_cli.cpp - HymoFS module management CLI wrapper
#include "hymo_cli.hpp"
#include "conf/config.hpp"
#include "core/executor.hpp"
#include "core/inventory.hpp"
#include "core/modules.hpp"
#include "core/planner.hpp"
#include "core/state.hpp"
#include "core/storage.hpp"
#include "core/sync.hpp"
#include "hymo_defs.hpp"
#include "hymo_utils.hpp"
#include "mount/hymofs.hpp"

#include <sys/mount.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace hymo {

// Forward declarations
static Config load_default_config();
static int cmd_mount();
static void segregate_custom_rules(MountPlan& plan, const fs::path& mirror_dir);

void print_hymo_help() {
    printf("USAGE: ksud hymo <SUBCOMMAND>\n\n");
    printf("SUBCOMMANDS:\n");
    printf("  mount           Mount all modules\n");
    printf("  reload          Reload HymoFS mappings\n");
    printf("  clear           Clear all HymoFS mappings\n");
    printf("  list            List all active HymoFS rules\n");
    printf("  version         Show HymoFS protocol version\n");
    printf("  modules         List active modules\n");
    printf("  storage         Show storage status\n");
    printf("  debug <on|off>  Enable/Disable kernel debug logging\n");
    printf("  add <mod_id>    Add module rules to HymoFS\n");
    printf("  delete <mod_id> Delete module rules from HymoFS\n");
    printf("  set-mode <mod_id> <mode>  Set mount mode for a module\n");
    printf("  show-config     Show current configuration\n");
    printf("  gen-config      Generate default config file\n");
    printf("  fix-mounts      Fix mount namespace issues\n");
    printf("  raw <cmd> ...   Execute raw HymoFS command\n");
}

static Config load_default_config() {
    try {
        return Config::load_default();
    } catch (const std::exception& e) {
        fs::path default_path = fs::path(BASE_DIR) / "config.toml";
        if (fs::exists(default_path)) {
            std::cerr << "Error loading config: " << e.what() << "\n";
        }
        return Config();
    }
}

int cmd_hymo(const std::vector<std::string>& args) {
    if (args.empty()) {
        print_hymo_help();
        return 1;
    }

    const std::string& subcmd = args[0];
    std::vector<std::string> subargs(args.begin() + 1, args.end());

    // Initialize logger
    Logger::getInstance().init(false, DAEMON_LOG_FILE);

    if (subcmd == "version") {
        if (HymoFS::is_available()) {
            int ver = HymoFS::get_protocol_version();
            printf("HymoFS Protocol Version: %d\n", HymoFS::EXPECTED_PROTOCOL_VERSION);
            printf("HymoFS Kernel Version: %d\n", ver);
        } else {
            printf("HymoFS not available.\n");
        }
        return 0;
    }

    if (subcmd == "status") {
        HymoFSStatus status = HymoFS::check_status();
        switch (status) {
        case HymoFSStatus::Available:
            printf("Available\n");
            break;
        case HymoFSStatus::NotPresent:
            printf("NotPresent\n");
            break;
        case HymoFSStatus::KernelTooOld:
            printf("KernelTooOld\n");
            break;
        case HymoFSStatus::ModuleTooOld:
            printf("ModuleTooOld\n");
            break;
        }
        return 0;
    }

    if (subcmd == "list") {
        if (HymoFS::is_available()) {
            std::string rules = HymoFS::get_active_rules();
            printf("%s", rules.c_str());
        } else {
            printf("HymoFS not available.\n");
        }
        return 0;
    }

    if (subcmd == "clear") {
        if (HymoFS::is_available()) {
            if (HymoFS::clear_rules()) {
                printf("Successfully cleared all HymoFS rules.\n");
                LOG_INFO("User manually cleared all HymoFS rules via CLI");

                RuntimeState state = load_runtime_state();
                state.hymofs_module_ids.clear();
                state.save();
            } else {
                fprintf(stderr, "Failed to clear HymoFS rules.\n");
                return 1;
            }
        } else {
            fprintf(stderr, "HymoFS not available.\n");
            return 1;
        }
        return 0;
    }

    if (subcmd == "debug") {
        if (subargs.empty()) {
            fprintf(stderr, "Usage: ksud hymo debug <on|off>\n");
            return 1;
        }
        std::string state = subargs[0];
        bool enable = (state == "on" || state == "1" || state == "true");

        if (HymoFS::is_available()) {
            if (HymoFS::set_debug(enable)) {
                printf("Kernel debug logging %s.\n", enable ? "enabled" : "disabled");
            } else {
                fprintf(stderr, "Failed to set kernel debug logging.\n");
                return 1;
            }
        } else {
            fprintf(stderr, "HymoFS not available.\n");
            return 1;
        }
        return 0;
    }

    if (subcmd == "fix-mounts") {
        if (HymoFS::is_available()) {
            if (HymoFS::fix_mounts()) {
                printf("Mount namespace fixed (mnt_id reordered).\n");
            } else {
                fprintf(stderr, "Failed to fix mount namespace.\n");
                return 1;
            }
        } else {
            fprintf(stderr, "HymoFS not available.\n");
            return 1;
        }
        return 0;
    }

    if (subcmd == "storage") {
        print_storage_status();
        return 0;
    }

    if (subcmd == "modules") {
        Config config = load_default_config();
        print_module_list(config);
        return 0;
    }

    if (subcmd == "show-config") {
        Config config = load_default_config();
        printf("{\n");
        printf("  \"moduledir\": \"%s\",\n", config.moduledir.string().c_str());
        printf("  \"tempdir\": \"%s\",\n", config.tempdir.string().c_str());
        printf("  \"mountsource\": \"%s\",\n", config.mountsource.c_str());
        printf("  \"verbose\": %s,\n", config.verbose ? "true" : "false");
        printf("  \"force_ext4\": %s,\n", config.force_ext4 ? "true" : "false");
        printf("  \"prefer_erofs\": %s,\n", config.prefer_erofs ? "true" : "false");
        printf("  \"disable_umount\": %s,\n", config.disable_umount ? "true" : "false");
        printf("  \"enable_nuke\": %s,\n", config.enable_nuke ? "true" : "false");
        printf("  \"ignore_protocol_mismatch\": %s,\n",
               config.ignore_protocol_mismatch ? "true" : "false");
        printf("  \"enable_kernel_debug\": %s,\n", config.enable_kernel_debug ? "true" : "false");
        printf("  \"enable_stealth\": %s,\n", config.enable_stealth ? "true" : "false");
        printf("  \"avc_spoof\": %s,\n", config.avc_spoof ? "true" : "false");
        printf("  \"hymofs_available\": %s,\n", HymoFS::is_available() ? "true" : "false");
        printf("  \"hymofs_status\": %d,\n", (int)HymoFS::check_status());
        printf("  \"partitions\": [");
        for (size_t i = 0; i < config.partitions.size(); ++i) {
            printf("\"%s\"", config.partitions[i].c_str());
            if (i < config.partitions.size() - 1)
                printf(", ");
        }
        printf("]\n");
        printf("}\n");
        return 0;
    }

    if (subcmd == "gen-config") {
        std::string output = subargs.empty() ? "config.toml" : subargs[0];
        Config().save_to_file(output);
        printf("Generated config: %s\n", output.c_str());
        return 0;
    }

    if (subcmd == "add") {
        if (subargs.empty()) {
            fprintf(stderr, "Error: Module ID required for add command\n");
            return 1;
        }
        Config config = load_default_config();
        std::string module_id = subargs[0];
        fs::path module_path = config.moduledir / module_id;

        if (!fs::exists(module_path)) {
            fprintf(stderr, "Error: Module not found: %s\n", module_id.c_str());
            return 1;
        }

        std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
        all_partitions.insert(all_partitions.end(), config.partitions.begin(),
                              config.partitions.end());

        std::sort(all_partitions.begin(), all_partitions.end());
        all_partitions.erase(std::unique(all_partitions.begin(), all_partitions.end()),
                             all_partitions.end());

        int success_count = 0;
        for (const auto& part : all_partitions) {
            fs::path src_dir = module_path / part;
            if (fs::exists(src_dir) && fs::is_directory(src_dir)) {
                fs::path target_base = fs::path("/") / part;
                if (HymoFS::add_rules_from_directory(target_base, src_dir)) {
                    success_count++;
                }
            }
        }

        if (success_count > 0) {
            printf("Successfully added module %s\n", module_id.c_str());
            RuntimeState state = load_runtime_state();
            bool already_active = false;
            for (const auto& id : state.hymofs_module_ids) {
                if (id == module_id) {
                    already_active = true;
                    break;
                }
            }
            if (!already_active) {
                state.hymofs_module_ids.push_back(module_id);
                state.save();
            }
        } else {
            printf("No content found to add for module %s\n", module_id.c_str());
        }
        return 0;
    }

    if (subcmd == "delete") {
        if (subargs.empty()) {
            fprintf(stderr, "Error: Module ID required for delete command\n");
            return 1;
        }
        Config config = load_default_config();
        std::string module_id = subargs[0];
        fs::path module_path = config.moduledir / module_id;

        std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
        all_partitions.insert(all_partitions.end(), config.partitions.begin(),
                              config.partitions.end());

        std::sort(all_partitions.begin(), all_partitions.end());
        all_partitions.erase(std::unique(all_partitions.begin(), all_partitions.end()),
                             all_partitions.end());

        int success_count = 0;
        for (const auto& part : all_partitions) {
            fs::path src_dir = module_path / part;
            if (fs::exists(src_dir) && fs::is_directory(src_dir)) {
                fs::path target_base = fs::path("/") / part;
                if (HymoFS::remove_rules_from_directory(target_base, src_dir)) {
                    success_count++;
                }
            }
        }

        if (success_count > 0) {
            printf("Successfully removed %d rules for module %s\n", success_count,
                   module_id.c_str());
            RuntimeState state = load_runtime_state();
            auto it = std::remove(state.hymofs_module_ids.begin(), state.hymofs_module_ids.end(),
                                  module_id);
            if (it != state.hymofs_module_ids.end()) {
                state.hymofs_module_ids.erase(it, state.hymofs_module_ids.end());
                state.save();
            }
        } else {
            printf("No active rules found or removed for module %s\n", module_id.c_str());
        }
        return 0;
    }

    if (subcmd == "set-mode") {
        if (subargs.size() < 2) {
            fprintf(stderr, "Usage: ksud hymo set-mode <mod_id> <mode>\n");
            return 1;
        }
        std::string mod_id = subargs[0];
        std::string mode = subargs[1];

        auto modes = load_module_modes();
        modes[mod_id] = mode;

        if (save_module_modes(modes)) {
            printf("Set mode for %s to %s\n", mod_id.c_str(), mode.c_str());
        } else {
            fprintf(stderr, "Failed to save module modes.\n");
            return 1;
        }
        return 0;
    }

    if (subcmd == "raw") {
        if (subargs.empty()) {
            fprintf(stderr, "Usage: ksud hymo raw <cmd> [args...]\n");
            return 1;
        }
        std::string cmd = subargs[0];
        bool success = false;

        if (cmd == "add") {
            if (subargs.size() < 3) {
                fprintf(stderr, "Usage: ksud hymo raw add <src> <target> [type]\n");
                return 1;
            }
            int type = 0;
            if (subargs.size() >= 4)
                type = std::stoi(subargs[3]);
            success = HymoFS::add_rule(subargs[1], subargs[2], type);
        } else if (cmd == "hide") {
            if (subargs.size() < 2) {
                fprintf(stderr, "Usage: ksud hymo raw hide <path>\n");
                return 1;
            }
            success = HymoFS::hide_path(subargs[1]);
        } else if (cmd == "delete") {
            if (subargs.size() < 2) {
                fprintf(stderr, "Usage: ksud hymo raw delete <src>\n");
                return 1;
            }
            success = HymoFS::delete_rule(subargs[1]);
        } else if (cmd == "merge") {
            if (subargs.size() < 3) {
                fprintf(stderr, "Usage: ksud hymo raw merge <src> <target>\n");
                return 1;
            }
            success = HymoFS::add_merge_rule(subargs[1], subargs[2]);
        } else if (cmd == "clear") {
            success = HymoFS::clear_rules();
        } else {
            fprintf(stderr, "Unknown raw command: %s\n", cmd.c_str());
            return 1;
        }

        if (success) {
            printf("Command executed successfully.\n");
        } else {
            fprintf(stderr, "Command failed.\n");
            return 1;
        }
        return 0;
    }

    if (subcmd == "reload") {
        Config config = load_default_config();
        Logger::getInstance().init(config.verbose, DAEMON_LOG_FILE);

        if (!HymoFS::is_available()) {
            fprintf(stderr, "HymoFS not available.\n");
            return 1;
        }

        LOG_INFO("Reloading HymoFS mappings...");

        std::string effective_mirror_path = hymo::HYMO_MIRROR_DEV;
        if (!config.mirror_path.empty()) {
            effective_mirror_path = config.mirror_path;
        } else if (!config.tempdir.empty()) {
            effective_mirror_path = config.tempdir.string();
        }
        const fs::path MIRROR_DIR = effective_mirror_path;

        auto module_list = scan_modules(config.moduledir, config);

        std::vector<Module> active_modules;
        std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
        for (const auto& part : config.partitions)
            all_partitions.push_back(part);

        for (const auto& mod : module_list) {
            if (fs::exists("/data/adb/hymo/run/hot_unmounted/" + mod.id)) {
                LOG_INFO("Skipping hot-unmounted module: " + mod.id);
                continue;
            }

            bool has_content = false;
            for (const auto& part : all_partitions) {
                if (has_files_recursive(mod.source_path / part)) {
                    has_content = true;
                    break;
                }
            }
            if (has_content)
                active_modules.push_back(mod);
        }
        module_list = active_modules;

        LOG_INFO("Syncing modules to mirror...");
        for (const auto& mod : module_list) {
            fs::path src = config.moduledir / mod.id;
            fs::path dst = MIRROR_DIR / mod.id;
            sync_dir(src, dst);
        }

        MountPlan plan = generate_plan(config, module_list, MIRROR_DIR);
        update_hymofs_mappings(config, module_list, MIRROR_DIR, plan);

        if (HymoFS::set_stealth(config.enable_stealth)) {
            LOG_INFO("Stealth mode set to: " +
                     std::string(config.enable_stealth ? "true" : "false"));
        }

        if (config.enable_stealth) {
            if (HymoFS::fix_mounts()) {
                LOG_INFO("Mount namespace fixed after reload.");
            }
        }

        RuntimeState state = load_runtime_state();
        if (state.storage_mode.empty())
            state.storage_mode = "hymofs";
        state.mount_point = MIRROR_DIR.string();
        state.hymofs_module_ids = plan.hymofs_module_ids;
        state.save();

        LOG_INFO("Reload complete.");
        printf("Reload complete.\n");
        return 0;
    }

    if (subcmd == "mount") {
        return cmd_mount();
    }

    fprintf(stderr, "Unknown hymo subcommand: %s\n", subcmd.c_str());
    print_hymo_help();
    return 1;
}

// Helper to segregate custom rules (Overlay/Magic) from HymoFS source tree
static void segregate_custom_rules(MountPlan& plan, const fs::path& mirror_dir) {
    fs::path staging_dir = mirror_dir / ".overlay_staging";

    // Process Overlay Ops
    for (auto& op : plan.overlay_ops) {
        for (auto& layer : op.lowerdirs) {
            std::string layer_str = layer.string();
            std::string mirror_str = mirror_dir.string();

            if (layer_str.find(mirror_str) == 0) {
                fs::path rel = fs::relative(layer, mirror_dir);
                fs::path target = staging_dir / rel;

                try {
                    if (fs::exists(layer)) {
                        fs::create_directories(target.parent_path());
                        fs::rename(layer, target);
                        layer = target;
                        LOG_DEBUG("Segregated custom rule source: " + layer_str + " -> " +
                                  target.string());
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("Failed to segregate custom rule source: " + layer_str + " - " +
                             e.what());
                }
            }
        }
    }

    // Process Magic Mounts
    for (auto& path : plan.magic_module_paths) {
        std::string path_str = path.string();
        std::string mirror_str = mirror_dir.string();

        if (path_str.find(mirror_str) == 0) {
            fs::path rel = fs::relative(path, mirror_dir);
            fs::path target = staging_dir / rel;

            try {
                if (fs::exists(path)) {
                    fs::create_directories(target.parent_path());
                    fs::rename(path, target);
                    path = target;
                    LOG_DEBUG("Segregated magic rule source: " + path_str + " -> " +
                              target.string());
                }
            } catch (const std::exception& e) {
                LOG_WARN("Failed to segregate magic rule source: " + path_str + " - " + e.what());
            }
        }
    }
}

// Full mount operation
static int cmd_mount() {
    Config config = load_default_config();
    Logger::getInstance().init(config.verbose, DAEMON_LOG_FILE);

    // Camouflage process
    if (!camouflage_process("kworker/u9:1")) {
        LOG_WARN("Failed to camouflage process");
    }

    LOG_INFO("Hymo Mount Starting...");

    if (config.disable_umount) {
        LOG_WARN("Namespace Detach (try_umount) is DISABLED.");
    }

    // Ensure runtime directory exists
    ensure_dir_exists(RUN_DIR);

    StorageHandle storage;
    MountPlan plan;
    ExecutionResult exec_result;
    std::vector<Module> module_list;

    HymoFSStatus hymofs_status = HymoFS::check_status();
    std::string warning_msg = "";
    bool hymofs_active = false;

    bool can_use_hymofs = (hymofs_status == HymoFSStatus::Available);

    if (!can_use_hymofs && config.ignore_protocol_mismatch) {
        if (hymofs_status == HymoFSStatus::KernelTooOld ||
            hymofs_status == HymoFSStatus::ModuleTooOld) {
            LOG_WARN("Forcing HymoFS despite protocol mismatch (ignore_protocol_mismatch=true)");
            can_use_hymofs = true;
            if (hymofs_status == HymoFSStatus::KernelTooOld) {
                warning_msg =
                    "⚠️Kernel version is lower than module version. Please update your kernel.";
            } else {
                warning_msg =
                    "⚠️Module version is lower than kernel version. Please update your module.";
            }
        } else {
            LOG_WARN("Cannot force HymoFS: Kernel module not present or error state (Status: " +
                     std::to_string((int)hymofs_status) + ")");
        }
    }

    if (can_use_hymofs) {
        // **HymoFS Fast Path**
        LOG_INFO("Mode: HymoFS Fast Path");

        // Determine Mirror Path
        std::string effective_mirror_path = hymo::HYMO_MIRROR_DEV;
        if (!config.mirror_path.empty()) {
            effective_mirror_path = config.mirror_path;
        } else if (!config.tempdir.empty()) {
            effective_mirror_path = config.tempdir.string();
        }

        // Apply Mirror Path to Kernel
        if (effective_mirror_path != hymo::HYMO_MIRROR_DEV) {
            if (HymoFS::set_mirror_path(effective_mirror_path)) {
                LOG_INFO("Applied custom mirror path: " + effective_mirror_path);
            } else {
                LOG_WARN("Failed to apply custom mirror path: " + effective_mirror_path);
            }
        }

        // Apply Kernel Debug Setting
        if (config.enable_kernel_debug) {
            if (HymoFS::set_debug(true)) {
                LOG_INFO("Kernel debug logging enabled via config.");
            } else {
                LOG_WARN("Failed to enable kernel debug logging (config).");
            }
        }

        // Apply Stealth Mode
        if (HymoFS::set_stealth(config.enable_stealth)) {
            LOG_INFO("Stealth mode set to: " +
                     std::string(config.enable_stealth ? "true" : "false"));
        } else {
            LOG_WARN("Failed to set stealth mode.");
        }

        // **Mirror Strategy (Tmpfs/Ext4)**
        const fs::path MIRROR_DIR = effective_mirror_path;
        fs::path img_path = fs::path(BASE_DIR) / "modules.img";
        bool mirror_success = false;

        try {
            // Setup storage with fallback
            try {
                storage =
                    setup_storage(MIRROR_DIR, img_path, config.force_ext4, config.prefer_erofs);
            } catch (const std::exception& e) {
                if (config.force_ext4) {
                    LOG_WARN("Force Ext4 failed: " + std::string(e.what()) +
                             ". Falling back to auto.");
                    storage = setup_storage(MIRROR_DIR, img_path, false, config.prefer_erofs);
                } else {
                    throw;
                }
            }
            LOG_INFO("Mirror storage setup successful. Mode: " + storage.mode);

            // Scan modules
            module_list = scan_modules(config.moduledir, config);

            // Filter modules with content
            std::vector<Module> active_modules;
            std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
            for (const auto& part : config.partitions)
                all_partitions.push_back(part);

            for (const auto& mod : module_list) {
                bool has_content = false;
                for (const auto& part : all_partitions) {
                    if (has_files_recursive(mod.source_path / part)) {
                        has_content = true;
                        break;
                    }
                }
                if (has_content) {
                    active_modules.push_back(mod);
                } else {
                    LOG_DEBUG("Skipping empty/irrelevant module for mirror: " + mod.id);
                }
            }

            module_list = active_modules;
            LOG_INFO("Syncing " + std::to_string(module_list.size()) +
                     " active modules to mirror...");

            bool sync_ok = true;
            for (const auto& mod : module_list) {
                fs::path src = config.moduledir / mod.id;
                fs::path dst = MIRROR_DIR / mod.id;
                if (!sync_dir(src, dst)) {
                    LOG_ERROR("Failed to sync module: " + mod.id);
                    sync_ok = false;
                }
            }

            if (sync_ok) {
                if (storage.mode == "ext4") {
                    finalize_storage_permissions(storage.mount_point);
                }

                mirror_success = true;
                hymofs_active = true;
                storage.mount_point = MIRROR_DIR;

                // Generate plan from MIRROR
                plan = generate_plan(config, module_list, MIRROR_DIR);

                // Segregate custom rules
                segregate_custom_rules(plan, MIRROR_DIR);

                // Update Kernel Mappings
                update_hymofs_mappings(config, module_list, MIRROR_DIR, plan);

                // Execute plan
                exec_result = execute_plan(plan, config);

                // Fix mount namespace if stealth enabled
                if (config.enable_stealth) {
                    if (HymoFS::fix_mounts()) {
                        LOG_INFO("Mount namespace fixed (mnt_id reordered) after mounting.");
                    } else {
                        LOG_WARN("Failed to fix mount namespace after mounting.");
                    }
                }
            } else {
                LOG_ERROR("Mirror sync failed. Aborting mirror strategy.");
                umount(MIRROR_DIR.c_str());
            }

        } catch (const std::exception& e) {
            LOG_ERROR("Failed to setup mirror storage: " + std::string(e.what()));
        }

        if (!mirror_success) {
            LOG_WARN("Mirror setup failed. Falling back to Magic Mount.");

            storage.mode = "magic_only";
            storage.mount_point = config.moduledir;

            module_list = scan_modules(config.moduledir, config);

            plan.overlay_ops.clear();
            plan.hymofs_module_ids.clear();
            plan.magic_module_paths.clear();

            std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
            for (const auto& part : config.partitions)
                all_partitions.push_back(part);

            for (const auto& mod : module_list) {
                bool has_content = false;
                for (const auto& part : all_partitions) {
                    if (has_files_recursive(mod.source_path / part)) {
                        has_content = true;
                        break;
                    }
                }

                if (has_content) {
                    plan.magic_module_paths.push_back(mod.source_path);
                    exec_result.magic_module_ids.push_back(mod.id);
                }
            }

            exec_result = execute_plan(plan, config);
        }

    } else {
        // **Legacy/Overlay Path**
        if (hymofs_status == HymoFSStatus::KernelTooOld) {
            LOG_WARN("HymoFS Protocol Mismatch! Kernel is too old.");
            warning_msg =
                "⚠️Kernel version is lower than module version. Please update your kernel.";
        } else if (hymofs_status == HymoFSStatus::ModuleTooOld) {
            LOG_WARN("HymoFS Protocol Mismatch! Module is too old.");
            warning_msg =
                "⚠️Module version is lower than kernel version. Please update your module.";
        }

        LOG_INFO("Mode: Standard Overlay/Magic (Copy)");

        fs::path mnt_base(FALLBACK_CONTENT_DIR);
        fs::path img_path = fs::path(BASE_DIR) / "modules.img";

        storage = setup_storage(mnt_base, img_path, config.force_ext4, config.prefer_erofs);

        module_list = scan_modules(config.moduledir, config);
        LOG_INFO("Scanned " + std::to_string(module_list.size()) + " active modules.");

        perform_sync(module_list, storage.mount_point, config);

        if (storage.mode == "ext4") {
            finalize_storage_permissions(storage.mount_point);
        }

        LOG_INFO("Generating mount plan...");
        plan = generate_plan(config, module_list, storage.mount_point);

        exec_result = execute_plan(plan, config);
    }

    LOG_INFO("Plan: " + std::to_string(exec_result.overlay_module_ids.size()) +
             " OverlayFS modules, " + std::to_string(exec_result.magic_module_ids.size()) +
             " Magic modules, " + std::to_string(plan.hymofs_module_ids.size()) +
             " HymoFS modules");

    // KSU Nuke (Stealth)
    bool nuke_active = false;
    if (storage.mode == "ext4" && config.enable_nuke) {
        LOG_INFO("Attempting to deploy Paw Pad (Stealth) via KernelSU...");
        if (ksu_nuke_sysfs(storage.mount_point.string())) {
            LOG_INFO("Success: Paw Pad active. Ext4 sysfs traces nuked.");
            nuke_active = true;
        } else {
            LOG_WARN("Paw Pad failed (KSU ioctl error)");
        }
    }

    // Save Runtime State
    RuntimeState state;
    state.storage_mode = storage.mode;
    state.mount_point = storage.mount_point.string();
    state.overlay_module_ids = exec_result.overlay_module_ids;
    state.magic_module_ids = exec_result.magic_module_ids;
    state.hymofs_module_ids = plan.hymofs_module_ids;
    state.nuke_active = nuke_active;

    // Populate active mounts
    if (!plan.hymofs_module_ids.empty()) {
        std::vector<std::string> all_parts = BUILTIN_PARTITIONS;
        for (const auto& p : config.partitions)
            all_parts.push_back(p);

        for (const auto& part : all_parts) {
            bool active = false;
            for (const auto& mod_id : plan.hymofs_module_ids) {
                for (const auto& m : module_list) {
                    if (m.id == mod_id) {
                        if (fs::exists(m.source_path / part)) {
                            active = true;
                            break;
                        }
                    }
                }
                if (active)
                    break;
            }
            if (active)
                state.active_mounts.push_back(part);
        }
    }

    // Add OverlayFS targets
    for (const auto& op : plan.overlay_ops) {
        fs::path p(op.target);
        std::string name = p.filename().string();
        bool exists = false;
        for (const auto& existing : state.active_mounts) {
            if (existing == name) {
                exists = true;
                break;
            }
        }
        if (!exists)
            state.active_mounts.push_back(name);
    }

    // Add Magic Mount targets
    if (!plan.magic_module_paths.empty()) {
        std::vector<std::string> all_parts = BUILTIN_PARTITIONS;
        for (const auto& p : config.partitions)
            all_parts.push_back(p);

        for (const auto& part : all_parts) {
            bool active = false;
            for (const auto& mod_id : exec_result.magic_module_ids) {
                for (const auto& m : module_list) {
                    if (m.id == mod_id) {
                        if (fs::exists(m.source_path / part)) {
                            active = true;
                            break;
                        }
                    }
                }
                if (active)
                    break;
            }

            bool exists = false;
            for (const auto& existing : state.active_mounts) {
                if (existing == part) {
                    exists = true;
                    break;
                }
            }
            if (active && !exists)
                state.active_mounts.push_back(part);
        }
    }

    // Update mismatch state
    if (hymofs_status == HymoFSStatus::KernelTooOld ||
        hymofs_status == HymoFSStatus::ModuleTooOld) {
        state.hymofs_mismatch = true;
        state.mismatch_message = warning_msg;
    } else if (config.ignore_protocol_mismatch && !warning_msg.empty()) {
        state.hymofs_mismatch = true;
        state.mismatch_message = warning_msg;
    }

    if (!state.save()) {
        LOG_ERROR("Failed to save runtime state");
    }

    LOG_INFO("Hymo Mount Completed.");
    printf("Mount completed successfully.\n");

    return 0;
}

}  // namespace hymo
