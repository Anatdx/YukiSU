// main.cpp - Main entry point
#include <getopt.h>
#include <sys/mount.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include "conf/config.hpp"
#include "core/executor.hpp"
#include "core/inventory.hpp"
#include "core/json.hpp"
#include "core/modules.hpp"
#include "core/planner.hpp"
#include "core/state.hpp"
#include "core/storage.hpp"
#include "core/sync.hpp"
#include "core/user_rules.hpp"
#include "core/webui.hpp"
#include "defs.hpp"
#include "hymo_cli.hpp"
#include "mount/hymofs.hpp"
#include "utils.hpp"

namespace fs = std::filesystem;
using namespace hymo;

struct CliOptions {
    std::string config_file;
    std::string command;
    fs::path moduledir;
    fs::path tempdir;
    std::string mountsource;
    bool verbose = false;
    std::vector<std::string> partitions;
    std::string output;
    std::vector<std::string> args;
};

static void print_help() {
    std::cout << "Usage: hymod [OPTIONS] <command> [args...]\n\n";
    std::cout << "Main Commands:\n";
    std::cout << "  mount              Mount all modules (default action)\n";
    std::cout << "  clear              Clear all HymoFS mappings\n";
    std::cout << "  fix-mounts         Fix mount namespace issues\n\n";

    std::cout << "Configuration Commands (config <subcommand>):\n";
    std::cout << "  config gen         Generate default config file\n";
    std::cout << "  config show        Show current configuration\n";
    std::cout << "  config sync-partitions  Scan and auto-add partitions\n";
    std::cout << "  config create-image [dir]  Create modules.img\n\n";

    std::cout << "Module Commands (module <subcommand>):\n";
    std::cout << "  module list        List all modules\n";
    std::cout << "  module add <id>    Add module to HymoFS\n";
    std::cout << "  module delete <id> Delete module from HymoFS\n";
    std::cout << "  module hot-mount <id>    Hot mount a module\n";
    std::cout << "  module hot-unmount <id>  Hot unmount a module\n";
    std::cout << "  module set-mode <id> <mode>  Set mount mode (auto/hymofs/overlay/magic/none)\n";
    std::cout << "  module add-rule <id> <path> <mode>  Add custom mount rule\n";
    std::cout << "  module remove-rule <id> <path>  Remove custom mount rule\n";
    std::cout << "  module check-conflicts  Check for file conflicts between modules\n\n";

    std::cout << "HymoFS Commands (hymofs <subcommand>):\n";
    std::cout << "  hymofs enable      Enable HymoFS (Protocol 11+)\n";
    std::cout << "  hymofs disable     Disable HymoFS\n";
    std::cout << "  hymofs list        List all active HymoFS rules\n";
    std::cout << "  hymofs version     Show HymoFS protocol version\n";
    std::cout << "  hymofs set-mirror <path>  Set custom mirror path\n";
    std::cout << "  hymofs raw <cmd> ...  Execute raw HymoFS command\n\n";

    std::cout << "API Commands (api <subcommand>) - JSON output for WebUI:\n";
    std::cout << "  api system         Complete system info with stats\n";
    std::cout << "  api storage        Storage usage information\n";
    std::cout << "  api mount-stats    Mount statistics\n";
    std::cout << "  api partitions     Detected partitions info\n\n";

    std::cout << "Privacy Commands (hide <subcommand>):\n";
    std::cout << "  hide list          List user-defined hide rules\n";
    std::cout << "  hide add <path>    Add a hide rule\n";
    std::cout << "  hide remove <path> Remove a hide rule\n\n";

    std::cout << "Debug Commands (debug <subcommand>):\n";
    std::cout << "  debug enable       Enable kernel debug logging\n";
    std::cout << "  debug disable      Disable kernel debug logging\n";
    std::cout << "  debug stealth on|off    Enable/disable stealth mode\n";
    std::cout << "  debug set-uname <release> <version>  Set kernel version spoofing\n\n";

    std::cout << "Options:\n";
    std::cout << "  -c, --config FILE       Config file path\n";
    std::cout << "  -m, --moduledir DIR     Module directory\n";
    std::cout << "  -t, --tempdir DIR       Temporary directory\n";
    std::cout << "  -s, --mountsource NAME  Mount source name\n";
    std::cout << "  -v, --verbose           Verbose logging\n";
    std::cout << "  -p, --partition NAME    Add partition (can be used multiple "
                 "times)\n";
    std::cout << "  -o, --output FILE       Output file (for gen-config)\n";
    std::cout << "  -h, --help              Show this help\n";
    std::cout << "\nExamples:\n";
    std::cout << "\nExamples:\n";
    std::cout << "  hymod mount                    # Mount all modules\n";
    std::cout << "  hymod config show              # Show configuration\n";
    std::cout << "  hymod module list              # List modules\n";
    std::cout << "  hymod api system               # Get system info (JSON)\n";
    std::cout << "  hymod hide add /path           # Add hide rule\n";
    std::cout << "  hymod debug enable             # Enable debug mode\n";
}

// Helper to segregate custom rules (Overlay/Magic) from HymoFS source tree
static void segregate_custom_rules(MountPlan& plan, const fs::path& mirror_dir) {
    fs::path staging_dir = mirror_dir / ".overlay_staging";

    // Process Overlay Ops ONLY
    // This function should only segregate overlay custom rules, not magic mount paths
    for (auto& op : plan.overlay_ops) {
        for (auto& layer : op.lowerdirs) {
            // Check if path starts with mirror_dir
            std::string layer_str = layer.string();
            std::string mirror_str = mirror_dir.string();

            if (layer_str.find(mirror_str) == 0) {
                // It is inside mirror. Move it to staging.
                // Construct relative path from mirror root
                fs::path rel = fs::relative(layer, mirror_dir);
                fs::path target = staging_dir / rel;

                try {
                    if (fs::exists(layer)) {
                        fs::create_directories(target.parent_path());
                        fs::rename(layer, target);
                        // Update the layer path in the plan
                        layer = target;
                        LOG_DEBUG("Segregated overlay custom rule: " + layer_str + " -> " +
                                  target.string());
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("Failed to segregate overlay custom rule: " + layer_str + " - " +
                             e.what());
                }
            }
        }
    }

    // DO NOT process Magic Mounts - they should use their original paths
    // Magic mount paths are module source directories, not overlay layers
}

static CliOptions parse_args(int argc, char* argv[]) {
    CliOptions opts;

    static struct option long_options[] = {{"config", required_argument, 0, 'c'},
                                           {"moduledir", required_argument, 0, 'm'},
                                           {"tempdir", required_argument, 0, 't'},
                                           {"mountsource", required_argument, 0, 's'},
                                           {"verbose", no_argument, 0, 'v'},
                                           {"partition", required_argument, 0, 'p'},
                                           {"output", required_argument, 0, 'o'},
                                           {"help", no_argument, 0, 'h'},
                                           {0, 0, 0, 0}};

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "c:m:t:s:vp:o:h", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'c':
            opts.config_file = optarg;
            break;
        case 'm':
            opts.moduledir = optarg;
            break;
        case 't':
            opts.tempdir = optarg;
            break;
        case 's':
            opts.mountsource = optarg;
            break;
        case 'v':
            opts.verbose = true;
            break;
        case 'p':
            opts.partitions.push_back(optarg);
            break;
        case 'o':
            opts.output = optarg;
            break;
        case 'h':
            print_help();
            exit(0);
        default:
            print_help();
            exit(1);
        }
    }

    if (optind < argc) {
        opts.command = argv[optind];
        optind++;
        while (optind < argc) {
            opts.args.push_back(argv[optind]);
            optind++;
        }
    }

    return opts;
}

static Config load_config(const CliOptions& opts) {
    if (!opts.config_file.empty()) {
        return Config::from_file(opts.config_file);
    }

    try {
        return Config::load_default();
    } catch (const std::exception& e) {
        fs::path default_path = fs::path(BASE_DIR) / CONFIG_FILENAME;
        if (fs::exists(default_path)) {
            std::cerr << "Error loading config: " << e.what() << "\n";
        }
        return Config();
    }
}

int hymo::run_hymo_main(int argc, char* argv[]) {
    try {
        CliOptions cli = parse_args(argc, argv);

        // Initialize logger globally for all commands
        Logger::getInstance().init(cli.verbose, cli.verbose, DAEMON_LOG_FILE);

        if (cli.command.empty()) {
            print_help();
            return 0;
        }

        // Map command string to enum for switch statement
        enum class Command {
            CONFIG,
            MODULE,
            HYMOFS,
            API,
            DEBUG,
            HIDE,
            CLEAR,
            FIX_MOUNTS,
            RAW,
            MOUNT,
            UNKNOWN
        };

        auto get_command = [](const std::string& cmd) -> Command {
            if (cmd == "config")
                return Command::CONFIG;
            if (cmd == "module")
                return Command::MODULE;
            if (cmd == "hymofs")
                return Command::HYMOFS;
            if (cmd == "api")
                return Command::API;
            if (cmd == "debug")
                return Command::DEBUG;
            if (cmd == "hide")
                return Command::HIDE;
            if (cmd == "clear")
                return Command::CLEAR;
            if (cmd == "fix-mounts")
                return Command::FIX_MOUNTS;
            if (cmd == "raw")
                return Command::RAW;
            if (cmd == "mount")
                return Command::MOUNT;
            return Command::UNKNOWN;
        };

        switch (get_command(cli.command)) {
        case Command::CONFIG: {
            if (cli.args.empty()) {
                std::cerr << "Usage: hymod config <gen|show|sync-partitions|create-image>\n";
                return 1;
            }
            std::string subcmd = cli.args[0];

            if (subcmd == "gen") {
                std::string output = cli.output.empty() ? CONFIG_FILENAME : cli.output;
                Config().save_to_file(output);
                std::cout << "Generated config: " << output << "\n";
                return 0;
            } else if (subcmd == "show") {
                Config config = load_config(cli);
                std::cout << "{\n";
                std::cout << "  \"moduledir\": \"" << config.moduledir.string() << "\",\n";
                std::cout << "  \"tempdir\": \"" << config.tempdir.string() << "\",\n";
                std::cout << "  \"mountsource\": \"" << config.mountsource << "\",\n";
                std::cout << "  \"mount_stage\": \"" << config.mount_stage << "\",\n";
                std::cout << "  \"debug\": " << (config.debug ? "true" : "false") << ",\n";
                std::cout << "  \"verbose\": " << (config.verbose ? "true" : "false") << ",\n";
                std::cout << "  \"fs_type\": \"" << filesystem_type_to_string(config.fs_type)
                          << "\",\n";
                std::cout << "  \"disable_umount\": " << (config.disable_umount ? "true" : "false")
                          << ",\n";
                std::cout << "  \"enable_nuke\": " << (config.enable_nuke ? "true" : "false")
                          << ",\n";
                std::cout << "  \"ignore_protocol_mismatch\": "
                          << (config.ignore_protocol_mismatch ? "true" : "false") << ",\n";
                std::cout << "  \"enable_kernel_debug\": "
                          << (config.enable_kernel_debug ? "true" : "false") << ",\n";
                std::cout << "  \"enable_stealth\": " << (config.enable_stealth ? "true" : "false")
                          << ",\n";
                std::cout << "  \"hymofs_enabled\": " << (config.hymofs_enabled ? "true" : "false")
                          << ",\n";
                std::cout << "  \"uname_release\": \"" << config.uname_release << "\",\n";
                std::cout << "  \"uname_version\": \"" << config.uname_version << "\",\n";
                std::cout << "  \"hymofs_available\": "
                          << (HymoFS::is_available() ? "true" : "false") << ",\n";
                std::cout << "  \"hymofs_status\": " << (int)HymoFS::check_status() << ",\n";
                std::cout << "  \"tmpfs_xattr_supported\": "
                          << (check_tmpfs_xattr() ? "true" : "false") << ",\n";
                std::cout << "  \"partitions\": [";
                for (size_t i = 0; i < config.partitions.size(); ++i) {
                    std::cout << "\"" << config.partitions[i] << "\"";
                    if (i < config.partitions.size() - 1)
                        std::cout << ", ";
                }
                std::cout << "]\n";
                std::cout << "}\n";
                return 0;
            } else if (subcmd == "sync-partitions") {
                Config config = load_config(cli);
                std::vector<std::string> candidates = scan_partition_candidates(config.moduledir);

                int added = 0;
                for (const auto& cand : candidates) {
                    // Check if already in config
                    bool exists = false;
                    for (const auto& p : config.partitions) {
                        if (p == cand) {
                            exists = true;
                            break;
                        }
                    }
                    // Check if builtin
                    for (const auto& p : BUILTIN_PARTITIONS) {
                        if (p == cand) {
                            exists = true;
                            break;
                        }
                    }

                    if (!exists) {
                        config.partitions.push_back(cand);
                        std::cout << "Added partition: " << cand << "\n";
                        added++;
                    }
                }

                if (added > 0) {
                    fs::path config_path = cli.config_file.empty()
                                               ? (fs::path(BASE_DIR) / CONFIG_FILENAME)
                                               : fs::path(cli.config_file);
                    if (config.save_to_file(config_path)) {
                        std::cout << "Updated config with " << added << " new partitions.\n";
                    } else {
                        std::cerr << "Failed to save config to " << config_path << "\n";
                        return 1;
                    }
                } else {
                    std::cout << "No new partitions found.\n";
                }
                return 0;
            } else if (subcmd == "create-image") {
                std::string dir = cli.args.size() >= 2 ? cli.args[1] : "/data/adb";
                fs::path img_dir(dir);
                if (create_image(img_dir)) {
                    std::cout << "Successfully created modules.img in " << dir << "\n";
                    LOG_INFO("Created modules.img via CLI");
                    return 0;
                } else {
                    std::cerr << "Failed to create modules.img\n";
                    LOG_ERROR("Failed to create modules.img via CLI");
                    return 1;
                }
            } else {
                std::cerr << "Unknown config subcommand: " << subcmd << "\n";
                std::cerr << "Available: gen, show, sync-partitions, create-image\n";
                return 1;
            }
            break;
        }

        case Command::MODULE: {
            if (cli.args.empty()) {
                std::cerr << "Usage: hymod module "
                             "<list|add|delete|hot-mount|hot-unmount|set-mode|add-rule|remove-rule|"
                             "check-conflicts>\n";
                return 1;
            }
            std::string subcmd = cli.args[0];
            Config config = load_config(cli);

            if (subcmd == "list") {
                print_module_list(config);
                return 0;
            } else if (subcmd == "add" || subcmd == "delete") {
                if (cli.args.size() < 2) {
                    std::cerr << "Usage: hymod module " << subcmd << " <module_id>\n";
                    return 1;
                }
                std::string module_id = cli.args[1];
                fs::path module_path = config.moduledir / module_id;

                std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
                all_partitions.insert(all_partitions.end(), config.partitions.begin(),
                                      config.partitions.end());
                std::sort(all_partitions.begin(), all_partitions.end());
                all_partitions.erase(std::unique(all_partitions.begin(), all_partitions.end()),
                                     all_partitions.end());

                int success_count = 0;
                if (subcmd == "add") {
                    if (!fs::exists(module_path)) {
                        std::cerr << "Error: Module not found: " << module_id << "\n";
                        return 1;
                    }

                    for (const auto& part : all_partitions) {
                        fs::path src_dir = module_path / part;
                        if (fs::exists(src_dir) && fs::is_directory(src_dir)) {
                            fs::path target_base = fs::path("/") / part;
                            if (HymoFS::add_rules_from_directory(target_base, src_dir)) {
                                if (config.verbose)
                                    std::cout << "Added rules for " << src_dir << "\n";
                                success_count++;
                            }
                        }
                    }

                    if (success_count > 0) {
                        std::cout << "Successfully added module " << module_id << "\n";
                        LOG_INFO("CLI: Added module " + module_id);

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
                        std::cout << "No content found to add for module " << module_id << "\n";
                    }
                } else {  // delete
                    for (const auto& part : all_partitions) {
                        fs::path src_dir = module_path / part;
                        if (fs::exists(src_dir) && fs::is_directory(src_dir)) {
                            fs::path target_base = fs::path("/") / part;
                            if (HymoFS::remove_rules_from_directory(target_base, src_dir)) {
                                if (config.verbose)
                                    std::cout << "Deleted rules for " << src_dir << "\n";
                                success_count++;
                            }
                        }
                    }

                    if (success_count > 0) {
                        std::cout << "Successfully removed " << success_count
                                  << " rules for module " << module_id << "\n";
                        LOG_INFO("CLI: Removed rules for module " + module_id);

                        RuntimeState state = load_runtime_state();
                        auto it = std::remove(state.hymofs_module_ids.begin(),
                                              state.hymofs_module_ids.end(), module_id);
                        if (it != state.hymofs_module_ids.end()) {
                            state.hymofs_module_ids.erase(it, state.hymofs_module_ids.end());
                            state.save();
                        }
                    } else {
                        std::cout << "No active rules found or removed for module " << module_id
                                  << "\n";
                    }
                }
                return 0;
            } else if (subcmd == "hot-mount" || subcmd == "hot-unmount") {
                if (cli.args.size() < 2) {
                    std::cerr << "Usage: hymod module " << subcmd << " <module_id>\n";
                    return 1;
                }
                std::string mod_id = cli.args[1];

                if (subcmd == "hot-mount") {
                    fs::path hot_unmounted = fs::path(RUN_DIR) / "hot_unmounted" / mod_id;
                    if (fs::exists(hot_unmounted))
                        fs::remove(hot_unmounted);

                    fs::path disabled_file = config.moduledir / mod_id / "disable";
                    if (fs::exists(disabled_file))
                        fs::remove(disabled_file);

                    fs::path module_path = config.moduledir / mod_id;
                    if (!fs::exists(module_path)) {
                        std::cerr << "Error: Module not found: " << mod_id << "\n";
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
                                if (config.verbose)
                                    std::cout << "Added rules for " << src_dir << "\n";
                                success_count++;
                            }
                        }
                    }

                    if (success_count > 0) {
                        std::cout << "Successfully added module " << mod_id << "\n";
                        LOG_INFO("CLI: Hot mounted module " + mod_id);

                        RuntimeState state = load_runtime_state();
                        bool already_active = false;
                        for (const auto& id : state.hymofs_module_ids) {
                            if (id == mod_id) {
                                already_active = true;
                                break;
                            }
                        }
                        if (!already_active) {
                            state.hymofs_module_ids.push_back(mod_id);
                            state.save();
                        }
                    } else {
                        std::cout << "No content found to add for module " << mod_id << "\n";
                    }
                } else {  // hot-unmount
                    fs::path hot_unmounted_dir = fs::path(RUN_DIR) / "hot_unmounted";
                    if (!fs::exists(hot_unmounted_dir))
                        fs::create_directories(hot_unmounted_dir);
                    std::ofstream(hot_unmounted_dir / mod_id).close();

                    std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
                    all_partitions.insert(all_partitions.end(), config.partitions.begin(),
                                          config.partitions.end());
                    std::sort(all_partitions.begin(), all_partitions.end());
                    all_partitions.erase(std::unique(all_partitions.begin(), all_partitions.end()),
                                         all_partitions.end());

                    fs::path module_path = config.moduledir / mod_id;

                    int success_count = 0;
                    for (const auto& part : all_partitions) {
                        fs::path src_dir = module_path / part;
                        fs::path target_base = fs::path("/") / part;
                        if (HymoFS::remove_rules_from_directory(target_base, src_dir)) {
                            success_count++;
                        }
                    }

                    if (success_count > 0) {
                        std::cout << "Successfully hot unmounted module " << mod_id << "\n";
                        LOG_INFO("CLI: Hot unmounted module " + mod_id);

                        RuntimeState state = load_runtime_state();
                        auto it = std::remove(state.hymofs_module_ids.begin(),
                                              state.hymofs_module_ids.end(), mod_id);
                        if (it != state.hymofs_module_ids.end()) {
                            state.hymofs_module_ids.erase(it, state.hymofs_module_ids.end());
                            state.save();
                        }
                    } else {
                        std::cout << "No active rules found for module " << mod_id << "\n";
                    }
                }
                return 0;
            } else if (subcmd == "set-mode") {
                if (cli.args.size() < 3) {
                    std::cerr << "Usage: hymod module set-mode <mod_id> <mode>\n";
                    return 1;
                }
                std::string mod_id = cli.args[1];
                std::string mode = cli.args[2];

                auto modes = load_module_modes();
                modes[mod_id] = mode;

                if (save_module_modes(modes)) {
                    std::cout << "Set mode for " << mod_id << " to " << mode << "\n";
                } else {
                    std::cerr << "Failed to save module modes.\n";
                    return 1;
                }
                return 0;
            } else if (subcmd == "add-rule") {
                if (cli.args.size() < 4) {
                    std::cerr << "Usage: hymod module add-rule <mod_id> <path> <mode>\n";
                    return 1;
                }
                std::string mod_id = cli.args[1];
                std::string path = cli.args[2];
                std::string mode = cli.args[3];

                auto rules = load_module_rules();
                bool found = false;
                for (auto& rule : rules[mod_id]) {
                    if (rule.path == path) {
                        rule.mode = mode;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    rules[mod_id].push_back({path, mode});
                }

                if (save_module_rules(rules)) {
                    std::cout << "Added rule for " << mod_id << ": " << path << " -> " << mode
                              << "\n";
                } else {
                    std::cerr << "Failed to save module rules.\n";
                    return 1;
                }
                return 0;
            } else if (subcmd == "remove-rule") {
                if (cli.args.size() < 3) {
                    std::cerr << "Usage: hymod module remove-rule <mod_id> <path>\n";
                    return 1;
                }
                std::string mod_id = cli.args[1];
                std::string path = cli.args[2];

                auto rules = load_module_rules();
                if (rules.find(mod_id) != rules.end()) {
                    auto& mod_rules = rules[mod_id];
                    auto it = std::remove_if(
                        mod_rules.begin(), mod_rules.end(),
                        [&path](const ModuleRuleConfig& r) { return r.path == path; });

                    if (it != mod_rules.end()) {
                        mod_rules.erase(it, mod_rules.end());
                        if (save_module_rules(rules)) {
                            std::cout << "Removed rule for " << mod_id << ": " << path << "\n";
                        } else {
                            std::cerr << "Failed to save module rules.\n";
                            return 1;
                        }
                    } else {
                        std::cout << "Rule not found.\n";
                    }
                } else {
                    std::cout << "Module not found in rules.\n";
                }
                return 0;
            } else if (subcmd == "check-conflicts") {
                // Check for file conflicts between modules
                auto module_list = scan_modules(config.moduledir, config);

                std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
                all_partitions.insert(all_partitions.end(), config.partitions.begin(),
                                      config.partitions.end());

                // Map: file path -> list of module IDs that modify it
                std::map<std::string, std::vector<std::string>> file_map;

                for (const auto& mod : module_list) {
                    // Skip disabled modules
                    if (fs::exists(mod.source_path / "disable"))
                        continue;

                    for (const auto& part : all_partitions) {
                        fs::path part_dir = mod.source_path / part;
                        if (!fs::exists(part_dir) || !fs::is_directory(part_dir))
                            continue;

                        // Walk through all files in this partition
                        try {
                            for (auto it = fs::recursive_directory_iterator(part_dir);
                                 it != fs::recursive_directory_iterator(); ++it) {
                                if (fs::is_regular_file(it->status()) ||
                                    fs::is_symlink(it->status())) {
                                    // Get relative path from partition root
                                    fs::path rel = fs::relative(it->path(), part_dir);
                                    std::string file_key = "/" + part + "/" + rel.string();
                                    file_map[file_key].push_back(mod.id);
                                }
                            }
                        } catch (const fs::filesystem_error& e) {
                            // Skip inaccessible directories
                            continue;
                        }
                    }
                }

                // Find conflicts (files modified by multiple modules)
                std::cout << "[";
                bool first = true;
                for (const auto& [file_path, module_ids] : file_map) {
                    if (module_ids.size() > 1) {
                        if (!first)
                            std::cout << ",";
                        first = false;

                        std::cout << "{\"file\":\"" << file_path << "\",\"modules\":[";
                        for (size_t i = 0; i < module_ids.size(); ++i) {
                            if (i > 0)
                                std::cout << ",";
                            std::cout << "\"" << module_ids[i] << "\"";
                        }
                        std::cout << "],\"message\":\"File '" << file_path << "' is modified by "
                                  << module_ids.size() << " modules: ";
                        for (size_t i = 0; i < module_ids.size(); ++i) {
                            if (i > 0)
                                std::cout << ", ";
                            std::cout << module_ids[i];
                        }
                        std::cout << "\"}";
                    }
                }
                std::cout << "]\n";
                return 0;
            } else {
                std::cerr << "Unknown module subcommand: " << subcmd << "\n";
                std::cerr << "Available: list, add, delete, hot-mount, hot-unmount, set-mode, "
                             "add-rule, remove-rule, check-conflicts\n";
                return 1;
            }
        }

        case Command::HYMOFS: {
            if (cli.args.empty()) {
                std::cerr << "Usage: hymod hymofs <enable|disable|list|version|set-mirror|raw>\n";
                return 1;
            }
            std::string subcmd = cli.args[0];

            if (subcmd == "enable" || subcmd == "disable") {
                bool enable = (subcmd == "enable");
                if (HymoFS::is_available()) {
                    if (HymoFS::set_enabled(enable)) {
                        std::cout << "HymoFS " << (enable ? "enabled" : "disabled") << ".\n";
                        LOG_INFO("HymoFS " + std::string(enable ? "enabled" : "disabled"));
                    } else {
                        std::cerr << "Failed to set HymoFS enable state.\n";
                        return 1;
                    }
                } else {
                    std::cerr << "HymoFS not available.\n";
                    return 1;
                }
                return 0;
            } else if (subcmd == "list") {
                json::Value root = json::Value::array();
                if (HymoFS::is_available()) {
                    std::string rules_str = HymoFS::get_active_rules();
                    std::istringstream iss(rules_str);
                    std::string line;
                    while (std::getline(iss, line)) {
                        if (line.empty())
                            continue;

                        json::Value rule = json::Value::object();
                        std::istringstream ls(line);
                        std::string type;
                        ls >> type;

                        std::string type_upper = type;
                        std::transform(type_upper.begin(), type_upper.end(), type_upper.begin(),
                                       ::toupper);

                        rule["type"] = json::Value(type_upper);

                        if (type_upper == "ADD" || type_upper == "MERGE") {
                            std::string target, source;
                            ls >> target >> source;
                            rule["target"] = json::Value(target);
                            rule["source"] = json::Value(source);
                        } else if (type_upper == "HIDE") {
                            std::string path;
                            ls >> path;
                            rule["path"] = json::Value(path);
                        } else {
                            std::string rest;
                            std::getline(ls, rest);
                            if (!rest.empty()) {
                                size_t first = rest.find_first_not_of(" ");
                                if (first != std::string::npos)
                                    rest = rest.substr(first);
                                rule["args"] = json::Value(rest);
                            }
                        }
                        root.push_back(rule);
                    }
                }
                std::cout << json::dump(root, 2) << "\n";
                return 0;
            } else if (subcmd == "version") {
                std::cout << "{\n";
                std::cout << "  \"protocol_version\": " << HymoFS::EXPECTED_PROTOCOL_VERSION
                          << ",\n";
                std::cout << "  \"hymofs_available\": "
                          << (HymoFS::is_available() ? "true" : "false") << ",\n";

                if (HymoFS::is_available()) {
                    int ver = HymoFS::get_protocol_version();
                    std::cout << "  \"kernel_version\": " << ver << ",\n";
                    std::cout << "  \"protocol_mismatch\": "
                              << (ver != HymoFS::EXPECTED_PROTOCOL_VERSION ? "true" : "false")
                              << ",\n";

                    std::string rules = HymoFS::get_active_rules();
                    std::set<std::string> active_modules;
                    std::istringstream iss(rules);
                    std::string line;
                    while (std::getline(iss, line)) {
                        size_t pos = line.find("/data/adb/modules/");
                        if (pos != std::string::npos) {
                            size_t start = pos + 18;
                            size_t end = line.find('/', start);
                            if (end != std::string::npos) {
                                std::string mod_id = line.substr(start, end - start);
                                active_modules.insert(mod_id);
                            }
                        }

                        pos = line.find("/dev/hymo_mirror/");
                        if (pos != std::string::npos) {
                            size_t start = pos + 17;
                            size_t end = line.find('/', start);
                            if (end != std::string::npos) {
                                std::string mod_id = line.substr(start, end - start);
                                active_modules.insert(mod_id);
                            }
                        }
                    }

                    std::cout << "  \"active_modules\": [";
                    bool first = true;
                    for (const auto& mod : active_modules) {
                        if (!first)
                            std::cout << ", ";
                        std::cout << "\"" << mod << "\"";
                        first = false;
                    }
                    std::cout << "],\n";
                } else {
                    std::cout << "  \"kernel_version\": 0,\n";
                    std::cout << "  \"protocol_mismatch\": false,\n";
                    std::cout << "  \"active_modules\": [],\n";
                }

                RuntimeState state = load_runtime_state();
                std::string mount_base =
                    state.mount_point.empty() ? "/dev/hymo_mirror" : state.mount_point;
                std::cout << "  \"mount_base\": \"" << mount_base << "\"\n";
                std::cout << "}\n";
                return 0;
            } else if (subcmd == "set-mirror") {
                if (cli.args.size() < 2) {
                    std::cerr << "Usage: hymod hymofs set-mirror <path>\n";
                    return 1;
                }
                std::string path = cli.args[1];
                Config config = load_config(cli);
                config.mirror_path = path;

                fs::path config_path = cli.config_file.empty()
                                           ? (fs::path(BASE_DIR) / "config.toml")
                                           : fs::path(cli.config_file);
                if (config.save_to_file(config_path)) {
                    std::cout << "Mirror path set to: " << path << "\n";
                    if (HymoFS::is_available()) {
                        if (HymoFS::set_mirror_path(path)) {
                            std::cout << "Applied mirror path to kernel.\n";
                        } else {
                            std::cerr << "Failed to apply mirror path to kernel.\n";
                        }
                    }
                } else {
                    std::cerr << "Failed to save config.\n";
                    return 1;
                }
                return 0;
            } else if (subcmd == "raw") {
                if (cli.args.size() < 2) {
                    std::cerr << "Usage: hymod hymofs raw <cmd> [args...]\n";
                    return 1;
                }
                std::string cmd = cli.args[1];
                bool success = false;

                if (cmd == "add") {
                    if (cli.args.size() < 4) {
                        std::cerr << "Usage: hymod hymofs raw add <src> <target> [type]\n";
                        return 1;
                    }
                    int type = 0;
                    if (cli.args.size() >= 5)
                        type = std::stoi(cli.args[4]);
                    success = HymoFS::add_rule(cli.args[2], cli.args[3], type);
                } else if (cmd == "hide") {
                    if (cli.args.size() < 3) {
                        std::cerr << "Usage: hymod hymofs raw hide <path>\n";
                        return 1;
                    }
                    success = HymoFS::hide_path(cli.args[2]);
                } else if (cmd == "delete") {
                    if (cli.args.size() < 3) {
                        std::cerr << "Usage: hymod hymofs raw delete <src>\n";
                        return 1;
                    }
                    success = HymoFS::delete_rule(cli.args[2]);
                } else if (cmd == "merge") {
                    if (cli.args.size() < 4) {
                        std::cerr << "Usage: hymod hymofs raw merge <src> <target>\n";
                        return 1;
                    }
                    success = HymoFS::add_merge_rule(cli.args[2], cli.args[3]);
                } else if (cmd == "clear") {
                    success = HymoFS::clear_rules();
                } else {
                    std::cerr << "Unknown raw command: " << cmd << "\n";
                    return 1;
                }

                if (success) {
                    std::cout << "Command executed successfully.\n";
                    LOG_INFO("Executed raw command: " + cmd);
                } else {
                    std::cerr << "Command failed.\n";
                    LOG_ERROR("Failed raw command: " + cmd);
                    return 1;
                }
                return 0;
            } else {
                std::cerr << "Unknown hymofs subcommand: " << subcmd << "\n";
                std::cerr << "Available: enable, disable, list, version, set-mirror, raw\n";
                return 1;
            }
        }

        case Command::API: {
            if (cli.args.empty()) {
                std::cerr << "Usage: hymod api <system|storage|mount-stats|partitions>\n";
                return 1;
            }
            std::string subcmd = cli.args[0];

            if (subcmd == "system") {
                std::cout << export_system_info_json() << std::endl;
            } else if (subcmd == "storage") {
                print_storage_status();
            } else if (subcmd == "mount-stats") {
                std::cout << export_mount_stats_json() << std::endl;
            } else if (subcmd == "partitions") {
                std::cout << export_partitions_json() << std::endl;
            } else {
                std::cerr << "Unknown api subcommand: " << subcmd << "\n";
                std::cerr << "Available: system, storage, mount-stats, partitions\n";
                return 1;
            }
            return 0;
        }

        case Command::DEBUG: {
            if (cli.args.empty()) {
                std::cerr << "Usage: hymod debug <enable|disable|stealth|set-uname>\n";
                return 1;
            }
            std::string subcmd = cli.args[0];

            if (subcmd == "enable" || subcmd == "disable") {
                bool enable = (subcmd == "enable");
                if (HymoFS::is_available()) {
                    if (HymoFS::set_debug(enable)) {
                        std::cout << "Kernel debug logging " << (enable ? "enabled" : "disabled")
                                  << ".\n";
                        LOG_INFO("Kernel debug logging " +
                                 std::string(enable ? "enabled" : "disabled"));
                    } else {
                        std::cerr << "Failed to set kernel debug logging.\n";
                        return 1;
                    }
                } else {
                    std::cerr << "HymoFS not available.\n";
                    return 1;
                }
                return 0;
            } else if (subcmd == "stealth") {
                if (cli.args.size() < 2) {
                    std::cerr << "Usage: hymod debug stealth <enable|disable>\n";
                    return 1;
                }
                std::string state = cli.args[1];
                bool enable =
                    (state == "enable" || state == "on" || state == "1" || state == "true");

                if (HymoFS::is_available()) {
                    if (HymoFS::set_stealth(enable)) {
                        std::cout << "Stealth mode " << (enable ? "enabled" : "disabled") << ".\n";
                        LOG_INFO("Stealth mode " + std::string(enable ? "enabled" : "disabled"));
                    } else {
                        std::cerr << "Failed to set stealth mode.\n";
                        return 1;
                    }
                } else {
                    std::cerr << "HymoFS not available.\n";
                    return 1;
                }
                return 0;
            } else if (subcmd == "set-uname") {
                if (cli.args.size() < 3) {
                    std::cerr << "Usage: hymod debug set-uname <release> <version>\n";
                    std::cerr << "Example: hymod debug set-uname \"5.15.0-generic\" \"#1 SMP "
                                 "PREEMPT ...\"\n";
                    return 1;
                }
                std::string release = cli.args[1];
                std::string version = cli.args[2];

                if (HymoFS::is_available()) {
                    Config config = load_config(cli);
                    config.uname_release = release;
                    config.uname_version = version;

                    fs::path config_path = cli.config_file.empty()
                                               ? (fs::path(BASE_DIR) / "config.toml")
                                               : fs::path(cli.config_file);

                    if (config.save_to_file(config_path)) {
                        std::cout << "Kernel version spoofing configured:\n";
                        std::cout << "  Release: " << release << "\n";
                        std::cout << "  Version: " << version << "\n";

                        if (HymoFS::set_uname(release, version)) {
                            std::cout << "Applied uname spoofing to kernel.\n";
                            LOG_INFO("Kernel uname updated: " + release + " " + version);
                        } else {
                            std::cerr << "Warning: Failed to apply uname to kernel.\n";
                        }
                    } else {
                        std::cerr << "Failed to save config.\n";
                        return 1;
                    }
                } else {
                    std::cerr << "HymoFS not available.\n";
                    return 1;
                }
                return 0;
            } else {
                std::cerr << "Unknown debug subcommand: " << subcmd << "\n";
                std::cerr << "Available: enable, disable, stealth, set-uname\n";
                return 1;
            }
        }

        case Command::HIDE: {
            if (cli.args.empty()) {
                std::cerr << "Usage: hymod hide <list|add|remove> [path]\n";
                return 1;
            }
            std::string subcmd = cli.args[0];

            if (subcmd == "list") {
                list_user_hide_rules();
            } else if (subcmd == "add") {
                if (cli.args.size() < 2) {
                    std::cerr << "Usage: hymod hide add <path>\n";
                    return 1;
                }
                std::string path = cli.args[1];
                return add_user_hide_rule(path) ? 0 : 1;
            } else if (subcmd == "remove") {
                if (cli.args.size() < 2) {
                    std::cerr << "Usage: hymod hide remove <path>\n";
                    return 1;
                }
                std::string path = cli.args[1];
                return remove_user_hide_rule(path) ? 0 : 1;
            } else {
                std::cerr << "Unknown hide subcommand: " << subcmd << "\n";
                std::cerr << "Available: list, add, remove\n";
                return 1;
            }
            return 0;
        }

        case Command::CLEAR: {
            if (HymoFS::is_available()) {
                if (HymoFS::clear_rules()) {
                    std::cout << "Successfully cleared all HymoFS rules.\n";
                    LOG_INFO("User manually cleared all HymoFS rules via CLI");

                    RuntimeState state = load_runtime_state();
                    state.hymofs_module_ids.clear();
                    state.save();
                } else {
                    std::cerr << "Failed to clear HymoFS rules.\n";
                    LOG_ERROR("Failed to clear HymoFS rules via CLI");
                    return 1;
                }
            } else {
                std::cerr << "HymoFS not available.\n";
                return 1;
            }
            return 0;
        }

        case Command::FIX_MOUNTS: {
            if (HymoFS::is_available()) {
                if (HymoFS::fix_mounts()) {
                    std::cout << "Mount namespace fixed (mnt_id reordered).\n";
                    LOG_INFO("Mount namespace fixed via CLI.");
                } else {
                    std::cerr << "Failed to fix mount namespace.\n";
                    return 1;
                }
            } else {
                std::cerr << "HymoFS not available.\n";
                return 1;
            }
            return 0;
        }

        case Command::RAW:
            // Raw commands moved to "hymofs raw" subcommand
            std::cerr << "Use 'hymod hymofs raw <cmd> ...' for raw commands\n";
            return 1;

        case Command::MOUNT:
            // Fall through to mount logic below
            break;

        case Command::UNKNOWN:
        default:
            std::cerr << "Unknown command: " << cli.command << "\n";
            print_help();
            return 1;
        }

        // Load and merge configuration
        Config config = load_config(cli);
        config.merge_with_cli(cli.moduledir, cli.tempdir, cli.mountsource, cli.verbose,
                              cli.partitions);

        // Re-initialize logger with merged config
        Logger::getInstance().init(config.debug, config.verbose, DAEMON_LOG_FILE);

        // Camouflage process
        if (!camouflage_process("kworker/u9:1")) {
            LOG_WARN("Failed to camouflage process");
        }

        LOG_INFO("Hymo Daemon Starting...");

        // Reset mount statistics at daemon start
        reset_mount_statistics();

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

        // Auto-select default tempdir if not set by user
        if (config.tempdir.empty()) {
            if (can_use_hymofs && config.hymofs_enabled) {
                // Use /dev/hymo_mirror when HymoFS is available and enabled
                config.tempdir = "/dev/hymo_mirror";
                LOG_INFO("Auto-selected tempdir: /dev/hymo_mirror (HymoFS mode)");
            } else {
                // Use /data/adb/hymo/img_mnt when HymoFS is not available or disabled
                config.tempdir = "/data/adb/hymo/img_mnt";
                LOG_INFO("Auto-selected tempdir: /data/adb/hymo/img_mnt (default mode)");
            }
        }

        if (!can_use_hymofs && config.ignore_protocol_mismatch) {
            if (hymofs_status == HymoFSStatus::KernelTooOld ||
                hymofs_status == HymoFSStatus::ModuleTooOld) {
                LOG_WARN("Forcing HymoFS despite protocol mismatch "
                         "(ignore_protocol_mismatch=true)");
                can_use_hymofs = true;
                if (hymofs_status == HymoFSStatus::KernelTooOld) {
                    warning_msg = "⚠️Kernel version is lower than module version. Please "
                                  "update your kernel.";
                } else if (hymofs_status == HymoFSStatus::ModuleTooOld) {
                    warning_msg = "⚠️Module version is lower than kernel version. Please "
                                  "update your module.";
                }
            } else {
                LOG_WARN("Cannot force HymoFS: Kernel module not present or error "
                         "state (Status: " +
                         std::to_string((int)hymofs_status) + ")");
            }
        }

        if (can_use_hymofs) {
            // **HymoFS Fast Path**
            LOG_INFO("Mode: HymoFS Fast Path");

            // Kernel defaults to hymofs_enabled=false; must set from config on every mount
            if (HymoFS::set_enabled(config.hymofs_enabled)) {
                LOG_INFO("HymoFS enabled=" + std::string(config.hymofs_enabled ? "true" : "false"));
            } else {
                LOG_WARN("Failed to set HymoFS enabled state.");
            }

            // Determine Mirror Path
            // Priority: config.mirror_path > config.tempdir > /dev/hymo_mirror
            // When HymoFS is available, we can mount to /dev safely because HymoFS
            // provides complete stealth and won't be detected
            std::string effective_mirror_path = hymo::HYMO_MIRROR_DEV;
            if (!config.mirror_path.empty()) {
                effective_mirror_path = config.mirror_path;
            } else if (!config.tempdir.empty()) {
                effective_mirror_path = config.tempdir.string();
            }

            // Apply Mirror Path to Kernel if using custom path
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
            if (config.enable_stealth) {
                if (HymoFS::set_stealth(config.enable_stealth)) {
                    LOG_INFO("Stealth mode set to: " +
                             std::string(config.enable_stealth ? "true" : "false"));
                } else {
                    LOG_WARN("Failed to set stealth mode.");
                }
            }

            // Apply Uname Spoofing if configured
            if (!config.uname_release.empty() || !config.uname_version.empty()) {
                if (HymoFS::set_uname(config.uname_release, config.uname_version)) {
                    LOG_INFO("Applied kernel version spoofing: release=\"" + config.uname_release +
                             "\", version=\"" + config.uname_version + "\"");
                } else {
                    LOG_WARN("Failed to apply kernel version spoofing.");
                }
            }

            // Scan modules first to determine mount strategy
            module_list = scan_modules(config.moduledir, config);

            // Filter modules: only consider modules with actual content
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
                    LOG_DEBUG("Skipping empty module: " + mod.id);
                }
            }

            module_list = active_modules;

            // **Mirror Strategy (Tmpfs/Ext4)**
            // To avoid SELinux/permission issues on /data, we mirror active modules
            // to a tmpfs or ext4 image and inject from there.
            const fs::path MIRROR_DIR = effective_mirror_path;
            fs::path img_path = fs::path(BASE_DIR) / "modules.img";
            bool mirror_success = false;

            try {
                // Handle Tmpfs -> EROFS -> Ext4 fallback
                try {
                    storage = setup_storage(MIRROR_DIR, img_path, config.fs_type);
                } catch (const std::exception& e) {
                    if (config.fs_type != FilesystemType::AUTO) {
                        LOG_WARN("Specific FS check failed, falling back to auto: " +
                                 std::string(e.what()));
                        storage = setup_storage(MIRROR_DIR, img_path, FilesystemType::AUTO);
                    } else {
                        throw;
                    }
                }
                LOG_INFO("Mirror storage setup: " + storage.mode);

                // EROFS is read-only: sync into a writable staging dir first, then build+mount.
                if (storage.mode == "erofs") {
                    fs::path staging_dir = fs::path(BASE_DIR) / "erofs_staging";
                    try {
                        if (fs::exists(staging_dir)) {
                            fs::remove_all(staging_dir);
                        }
                    } catch (...) {
                        LOG_WARN("Failed to clean EROFS staging dir");
                    }
                    ensure_dir_exists(staging_dir);

                    LOG_INFO("Syncing " + std::to_string(module_list.size()) +
                             " active modules to EROFS staging...");

                    bool sync_ok = true;
                    for (const auto& mod : module_list) {
                        fs::path src = config.moduledir / mod.id;
                        fs::path dst = staging_dir / mod.id;
                        if (!sync_dir(src, dst)) {
                            LOG_ERROR("Failed to sync module: " + mod.id);
                            sync_ok = false;
                        }
                    }

                    if (!sync_ok) {
                        LOG_ERROR("EROFS staging sync failed. Aborting mirror strategy.");
                        umount(MIRROR_DIR.c_str());
                    } else {
                        storage = setup_erofs_storage(MIRROR_DIR, staging_dir,
                                                      fs::path(BASE_DIR) / "modules.erofs");
                        mirror_success = true;
                        hymofs_active = true;

                        // Plan should be generated from the mirrored storage root.
                        plan = generate_plan(config, module_list, MIRROR_DIR);
                        segregate_custom_rules(plan, MIRROR_DIR);
                        update_hymofs_mappings(config, module_list, MIRROR_DIR, plan);
                        exec_result = execute_plan(plan, config, hymofs_active);

                        if (config.enable_stealth) {
                            if (HymoFS::fix_mounts()) {
                                LOG_INFO("Mount namespace fixed (mnt_id reordered).");
                            } else {
                                LOG_WARN("Failed to fix mount namespace.");
                            }
                        }
                    }
                } else {
                    // module_list already filtered above, just sync to mirror
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
                        // If using ext4 image, we need to fix permissions after sync
                        if (storage.mode == "ext4") {
                            finalize_storage_permissions(storage.mount_point);
                        }

                        mirror_success = true;
                        hymofs_active = true;

                        // Plan should be generated from the mirrored storage root.
                        plan = generate_plan(config, module_list, MIRROR_DIR);

                        // Prepare plan and update mappings
                        segregate_custom_rules(plan, MIRROR_DIR);
                        update_hymofs_mappings(config, module_list, MIRROR_DIR, plan);
                        exec_result = execute_plan(plan, config, hymofs_active);

                        if (config.enable_stealth) {
                            if (HymoFS::fix_mounts()) {
                                LOG_INFO("Mount namespace fixed (mnt_id reordered).");
                            } else {
                                LOG_WARN("Failed to fix mount namespace.");
                            }
                        }
                    } else {
                        LOG_ERROR("Mirror sync failed. Aborting mirror strategy.");
                        umount(MIRROR_DIR.c_str());
                    }
                }

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to setup mirror storage: " + std::string(e.what()));
            }

            if (!mirror_success) {
                LOG_WARN("Mirror setup failed. Falling back to Magic Mount.");

                // Fallback to Magic Mount using source directory directly
                storage.mode = "tmpfs";
                storage.mount_point = config.moduledir;

                module_list = scan_modules(config.moduledir, config);

                // Manually construct a Magic Mount plan
                plan.overlay_ops.clear();
                plan.hymofs_module_ids.clear();
                plan.magic_module_paths.clear();

                for (const auto& mod : module_list) {
                    // Check if module has content
                    bool has_content = false;
                    std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
                    for (const auto& part : config.partitions)
                        all_partitions.push_back(part);

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

                // Execute plan
                exec_result = execute_plan(plan, config, hymofs_active);
            }

        } else {
            // **Legacy/Overlay Path**
            if (hymofs_status == HymoFSStatus::KernelTooOld) {
                LOG_WARN("HymoFS Protocol Mismatch! Kernel is too old.");
                warning_msg = "⚠️Kernel version is lower than module version. Please "
                              "update your kernel.";
            } else if (hymofs_status == HymoFSStatus::ModuleTooOld) {
                LOG_WARN("HymoFS Protocol Mismatch! Module is too old.");
                warning_msg = "⚠️Module version is lower than kernel version. Please "
                              "update your module.";
            }

            LOG_INFO("Mode: Standard Overlay/Magic (Copy)");

            // **Step 1: Setup Storage**
            fs::path mnt_base(FALLBACK_CONTENT_DIR);
            fs::path img_path = fs::path(BASE_DIR) / "modules.img";

            storage = setup_storage(mnt_base, img_path, config.fs_type);

            // **Step 2: Scan Modules**
            module_list = scan_modules(config.moduledir, config);
            LOG_INFO("Scanned " + std::to_string(module_list.size()) + " active modules.");

            // **Step 3: Sync Content**
            if (storage.mode == "erofs") {
                // EROFS is read-only: stage content first, then build+mount.
                fs::path staging_dir = fs::path(BASE_DIR) / "erofs_staging";
                try {
                    if (fs::exists(staging_dir)) {
                        fs::remove_all(staging_dir);
                    }
                } catch (...) {
                    LOG_WARN("Failed to clean EROFS staging dir");
                }
                ensure_dir_exists(staging_dir);

                perform_sync(module_list, staging_dir, config);
                storage = setup_erofs_storage(mnt_base, staging_dir,
                                              fs::path(BASE_DIR) / "modules.erofs");
            } else {
                perform_sync(module_list, storage.mount_point, config);

                // **FIX 1: Fix permissions after sync**
                if (storage.mode == "ext4") {
                    finalize_storage_permissions(storage.mount_point);
                }
            }

            // **Step 4: Generate Plan**
            LOG_INFO("Generating mount plan...");
            plan = generate_plan(config, module_list, storage.mount_point);

            // **Step 5: Execute Plan**
            exec_result = execute_plan(plan, config, hymofs_active);
        }

        LOG_INFO("Plan: " + std::to_string(exec_result.overlay_module_ids.size()) +
                 " OverlayFS modules, " + std::to_string(exec_result.magic_module_ids.size()) +
                 " Magic modules, " + std::to_string(plan.hymofs_module_ids.size()) +
                 " HymoFS modules");

        // **Step 6: KSU Nuke (Stealth)**
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

        // **Step 8: Save Runtime State**
        RuntimeState state;
        state.storage_mode = storage.mode;
        state.mount_point = storage.mount_point.string();
        state.overlay_module_ids = exec_result.overlay_module_ids;
        state.magic_module_ids = exec_result.magic_module_ids;
        state.hymofs_module_ids = plan.hymofs_module_ids;
        state.nuke_active = nuke_active;
        state.pid = getpid();

        // Track active mount partitions
        if (!plan.hymofs_module_ids.empty()) {
            std::vector<std::string> all_parts = BUILTIN_PARTITIONS;
            for (const auto& p : config.partitions)
                all_parts.push_back(p);

            for (const auto& part : all_parts) {
                bool active = false;
                // Check if any HymoFS module has this partition
                for (const auto& mod_id : plan.hymofs_module_ids) {
                    // Find module by ID (inefficient but works)
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

        // Also add OverlayFS targets
        for (const auto& op : plan.overlay_ops) {
            // op.target is like "/system"
            fs::path p(op.target);
            std::string name = p.filename().string();
            // Avoid duplicates
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

        // Track Magic Mount targets
        if (!plan.magic_module_paths.empty()) {
            std::vector<std::string> all_parts = BUILTIN_PARTITIONS;
            for (const auto& p : config.partitions)
                all_parts.push_back(p);

            for (const auto& part : all_parts) {
                bool active = false;
                // Check if any Magic module has this partition
                for (const auto& mod_id : plan.magic_module_ids) {
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

                // Avoid duplicates
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

        // Update mismatch state for UI
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

        // Update module description
        update_module_description(true, storage.mode, nuke_active,
                                  exec_result.overlay_module_ids.size(),
                                  exec_result.magic_module_ids.size(),
                                  plan.hymofs_module_ids.size(), warning_msg, hymofs_active);

        // Apply HymoFS Enable/Disable at the very end to avoid race conditions/crashes during setup
        if (can_use_hymofs) {
            if (HymoFS::set_enabled(config.hymofs_enabled)) {
                LOG_INFO("HymoFS enabled set to: " +
                         std::string(config.hymofs_enabled ? "true" : "false"));
            } else {
                LOG_WARN("Failed to set HymoFS enabled state.");
            }
        }

        LOG_INFO("Hymo Completed.");
    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << "\n";
        LOG_ERROR("Fatal Error: " + std::string(e.what()));
        // Update with failure emoji
        update_module_description(false, "error", false, 0, 0, 0, "", false);
        return 1;
    }
    return 0;
}