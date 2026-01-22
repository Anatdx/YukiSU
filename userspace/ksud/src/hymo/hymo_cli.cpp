// hymo_cli.cpp - HymoFS module management CLI
// Auto-synced from hymo/src/main.cpp
//
// Changes from standalone version:
// - int main() -> int hymo_main()
// - print_help() -> print_hymo_help()
// - "hymod" -> "ksud hymo" in help text
//
// NOTE: This file is auto-generated. Manual edits will be overwritten!

// main.cpp - Main entry point
#include <getopt.h>
#include <sys/mount.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
#include "hymo_defs.hpp"
#include "hymo_utils.hpp"
#include "mount/hymofs.hpp"

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

void print_hymo_help() {
    std::cout << "USAGE: ksud hymo [OPTIONS] [COMMAND]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  mount           Mount all modules (Default action previously)\n";
    std::cout << "  gen-config      Generate default config file\n";
    std::cout << "  show-config     Show current configuration\n";
    std::cout << "  storage         Show storage status\n";
    std::cout << "  modules         List active modules\n";
    std::cout << "  reload          Reload HymoFS mappings\n";
    std::cout << "  clear           Clear all HymoFS mappings\n";
    std::cout << "  version         Show HymoFS protocol and config version\n";
    std::cout << "  list            List all active HymoFS rules\n";
    std::cout << "  hide list       List user-defined hide rules\n";
    std::cout << "  hide add <path> Add a user-defined hide rule\n";
    std::cout << "  hide remove <path> Remove a user-defined hide rule\n";
    std::cout << "  debug <on|off>  Enable/Disable kernel debug logging\n";
    std::cout << "  stealth <on|off> Enable/Disable stealth mode\n";
    std::cout << "  hymofs <on|off> Enable/Disable HymoFS (Protocol 11+)\n";
    std::cout << "  set-uname <release> <version> Set kernel version spoofing\n";
    std::cout << "  raw <cmd> ...   Execute raw HymoFS command "
                 "(add/hide/delete/merge)\n";
    std::cout << "  add <mod_id>    Add module rules to HymoFS\n";
    std::cout << "  delete <mod_id> Delete module rules from HymoFS\n";
    std::cout << "  set-mode <mod_id> <mode>  Set mount mode for a module (auto, "
                 "hymofs, overlay, magic, none)\n";
    std::cout << "  add-rule <mod_id> <path> <mode> Add a custom mount rule for "
                 "a module\n";
    std::cout << "  remove-rule <mod_id> <path> Remove a custom mount rule for a "
                 "module\n";
    std::cout << "  set-mirror <path> Set custom mirror path for HymoFS\n";
    std::cout << "  fix-mounts      Fix mount namespace issues (reorder mnt_id)\n";
    std::cout << "  sync-partitions Scan modules and auto-add new partitions to "
                 "config\n";
    std::cout << "  create-image [dir] Create modules.img in specified directory (or default)\n";
    std::cout << "  hot-mount <mod_id> Hot mount a module (live reload)\n";
    std::cout << "  hot-unmount <mod_id> Hot unmount a module (live reload)\n\n";
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
}

// Helper to segregate custom rules (Overlay/Magic) from HymoFS source tree
static void segregate_custom_rules(MountPlan& plan, const fs::path& mirror_dir) {
    fs::path staging_dir = mirror_dir / ".overlay_staging";

    // Process Overlay Ops
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
    // plan.magic_module_paths is a vector of paths
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
            print_hymo_help();
            exit(0);
        default:
            print_hymo_help();
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

int hymo_main(int argc, char** argv) {
    try {
        CliOptions cli = parse_args(argc, argv);

        // Initialize logger globally for all commands
        Logger::getInstance().init(cli.verbose, DAEMON_LOG_FILE);

        if (cli.command.empty()) {
            print_hymo_help();
            return 0;
        }

        // Process commands
        if (!cli.command.empty()) {
            if (cli.command == "gen-config") {
                std::string output = cli.output.empty() ? CONFIG_FILENAME : cli.output;
                Config().save_to_file(output);
                std::cout << "Generated config: " << output << "\n";
                return 0;
            } else if (cli.command == "show-config") {
                Config config = load_config(cli);
                std::cout << "{\n";
                std::cout << "  \"moduledir\": \"" << config.moduledir.string() << "\",\n";
                std::cout << "  \"tempdir\": \"" << config.tempdir.string() << "\",\n";
                std::cout << "  \"mountsource\": \"" << config.mountsource << "\",\n";
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
            } else if (cli.command == "sync-partitions") {
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
            } else if (cli.command == "create-image") {
                fs::path target_dir = cli.args.empty() ? fs::path(BASE_DIR) : fs::path(cli.args[0]);
                if (create_image(target_dir)) {
                    std::cout << "Successfully created image at " << target_dir << "/modules.img\n";
                    return 0;
                } else {
                    std::cerr << "Failed to create image\n";
                    return 1;
                }
            } else if (cli.command == "hot-mount") {
                if (cli.args.empty()) {
                    std::cerr << "USAGE: ksud hymo hot-mount <module_id>\n";
                    return 1;
                }
                std::string mod_id = cli.args[0];

                fs::path hot_unmounted = fs::path(RUN_DIR) / "hot_unmounted" / mod_id;
                if (fs::exists(hot_unmounted))
                    fs::remove(hot_unmounted);

                Config config = load_config(cli);
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
                return 0;

            } else if (cli.command == "hot-unmount") {
                if (cli.args.empty()) {
                    std::cerr << "USAGE: ksud hymo hot-unmount <module_id>\n";
                    return 1;
                }
                std::string mod_id = cli.args[0];

                // 1. Create marker
                fs::path hot_unmounted_dir = fs::path(RUN_DIR) / "hot_unmounted";
                if (!fs::exists(hot_unmounted_dir))
                    fs::create_directories(hot_unmounted_dir);
                std::ofstream(hot_unmounted_dir / mod_id).close();

                // 2. Delete logic
                Config config = load_config(cli);
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
                return 0;

            } else if (cli.command == "add") {
                Config config = load_config(cli);
                if (cli.args.empty()) {
                    std::cerr << "Error: Module ID required for add command\n";
                    return 1;
                }
                std::string module_id = cli.args[0];
                fs::path module_path = config.moduledir / module_id;

                if (!fs::exists(module_path)) {
                    std::cerr << "Error: Module not found: " << module_id << "\n";
                    return 1;
                }

                std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
                all_partitions.insert(all_partitions.end(), config.partitions.begin(),
                                      config.partitions.end());

                // Deduplicate
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
                                std::cout << "Added rules for " << src_dir << " to " << target_base
                                          << "\n";
                            success_count++;
                        }
                    }
                }

                if (success_count > 0) {
                    std::cout << "Successfully added module " << module_id << "\n";
                    LOG_INFO("CLI: Added module " + module_id);

                    // Update runtime state
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
                return 0;
            } else if (cli.command == "delete") {
                Config config = load_config(cli);
                if (cli.args.empty()) {
                    std::cerr << "Error: Module ID required for delete command\n";
                    return 1;
                }
                std::string module_id = cli.args[0];
                fs::path module_path = config.moduledir / module_id;

                std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
                all_partitions.insert(all_partitions.end(), config.partitions.begin(),
                                      config.partitions.end());

                // Deduplicate
                std::sort(all_partitions.begin(), all_partitions.end());
                all_partitions.erase(std::unique(all_partitions.begin(), all_partitions.end()),
                                     all_partitions.end());

                int success_count = 0;
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
                    std::cout << "Successfully removed " << success_count << " rules for module "
                              << module_id << "\n";
                    LOG_INFO("CLI: Removed rules for module " + module_id);

                    // Update runtime state
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
                return 0;
            } else if (cli.command == "storage") {
                print_storage_status();
                return 0;
            } else if (cli.command == "modules") {
                Config config = load_config(cli);
                print_module_list(config);
                return 0;
            } else if (cli.command == "clear") {
                if (HymoFS::is_available()) {
                    if (HymoFS::clear_rules()) {
                        std::cout << "Successfully cleared all HymoFS rules.\n";
                        LOG_INFO("User manually cleared all HymoFS rules via CLI");

                        // Update runtime state to reflect cleared state
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
            } else if (cli.command == "version") {
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

                    // Parse active rules to get module list
                    std::string rules = HymoFS::get_active_rules();
                    std::set<std::string> active_modules;
                    std::istringstream iss(rules);
                    std::string line;
                    while (std::getline(iss, line)) {
                        // Support standard path
                        // Parse lines like: "ADD /data/adb/modules/module_id/..."
                        size_t pos = line.find("/data/adb/modules/");
                        if (pos != std::string::npos) {
                            size_t start = pos + 18;  // length of "/data/adb/modules/"
                            size_t end = line.find('/', start);
                            if (end != std::string::npos) {
                                std::string mod_id = line.substr(start, end - start);
                                active_modules.insert(mod_id);
                            }
                        }

                        // Support mirror path
                        // Parse lines like: "... /dev/hymo_mirror/module_id/..."
                        pos = line.find("/dev/hymo_mirror/");
                        if (pos != std::string::npos) {
                            size_t start = pos + 17;  // length of "/dev/hymo_mirror/"
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
            } else if (cli.command == "list") {
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

                        // Normalize type to Uppercase for consistent JSON output
                        std::string type_upper = type;
                        std::transform(type_upper.begin(), type_upper.end(), type_upper.begin(),
                                       ::toupper);

                        rule["type"] = json::Value(type_upper);

                        // Parse remaining args based on type (Case-insensitive check)
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
                            // Catch-all for unknown
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
            } else if (cli.command == "debug") {
                if (cli.args.empty()) {
                    std::cerr << "USAGE: ksud hymo debug <on|off>\n";
                    return 1;
                }
                std::string state = cli.args[0];
                bool enable = (state == "on" || state == "1" || state == "true");

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
            } else if (cli.command == "stealth") {
                if (cli.args.empty()) {
                    std::cerr << "USAGE: ksud hymo stealth <on|off>\n";
                    return 1;
                }
                std::string state = cli.args[0];
                bool enable = (state == "on" || state == "1" || state == "true");

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
            } else if (cli.command == "hymofs") {
                if (cli.args.empty()) {
                    std::cerr << "USAGE: ksud hymo hymofs <on|off>\n";
                    return 1;
                }
                std::string state = cli.args[0];
                bool enable = (state == "on" || state == "1" || state == "true");

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
            } else if (cli.command == "set-uname") {
                // set-uname <release> <version>
                // Allows setting empty strings to clear the spoofing
                std::string release = (cli.args.size() > 0) ? cli.args[0] : "";
                std::string version = (cli.args.size() > 1) ? cli.args[1] : "";

                if (HymoFS::is_available()) {
                    // Update config first for persistence
                    Config config = load_config(cli);
                    config.uname_release = release;
                    config.uname_version = version;

                    fs::path config_path = cli.config_file.empty()
                                               ? (fs::path(BASE_DIR) / "config.toml")
                                               : fs::path(cli.config_file);
                    config.save_to_file(config_path);

                    // Then apply to kernel
                    if (HymoFS::set_uname(release, version)) {
                        std::cout << "Kernel uname spoofing updated.\n";
                        LOG_INFO("Kernel uname updated: " + release + " " + version);
                    } else {
                        std::cerr << "Failed to set kernel uname spoofing.\n";
                        return 1;
                    }
                } else {
                    std::cerr << "HymoFS not available.\n";
                    return 1;
                }
                return 0;
            } else if (cli.command == "hide") {
                // Hide path management commands
                if (cli.args.empty()) {
                    std::cerr << "USAGE: ksud hymo hide <list|add|remove> [path]\n";
                    return 1;
                }

                std::string subcmd = cli.args[0];

                if (subcmd == "list") {
                    list_user_hide_rules();
                    return 0;
                } else if (subcmd == "add") {
                    if (cli.args.size() < 2) {
                        std::cerr << "USAGE: ksud hymo hide add <path>\n";
                        return 1;
                    }
                    std::string path = cli.args[1];
                    return add_user_hide_rule(path) ? 0 : 1;
                } else if (subcmd == "remove") {
                    if (cli.args.size() < 2) {
                        std::cerr << "USAGE: ksud hymo hide remove <path>\n";
                        return 1;
                    }
                    std::string path = cli.args[1];
                    return remove_user_hide_rule(path) ? 0 : 1;
                } else {
                    std::cerr << "Unknown hide subcommand: " << subcmd << "\n";
                    std::cerr << "Available: list, add, remove\n";
                    return 1;
                }
            } else if (cli.command == "fix-mounts") {
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
            } else if (cli.command == "raw") {
                if (cli.args.empty()) {
                    std::cerr << "USAGE: ksud hymo raw <cmd> [args...]\n";
                    return 1;
                }
                std::string cmd = cli.args[0];
                bool success = false;

                if (cmd == "add") {
                    if (cli.args.size() < 3) {
                        std::cerr << "USAGE: ksud hymo raw add <src> <target> [type]\n";
                        return 1;
                    }
                    int type = 0;
                    if (cli.args.size() >= 4)
                        type = std::stoi(cli.args[3]);
                    success = HymoFS::add_rule(cli.args[1], cli.args[2], type);
                } else if (cmd == "hide") {
                    if (cli.args.size() < 2) {
                        std::cerr << "USAGE: ksud hymo raw hide <path>\n";
                        return 1;
                    }
                    success = HymoFS::hide_path(cli.args[1]);
                } else if (cmd == "delete") {
                    if (cli.args.size() < 2) {
                        std::cerr << "USAGE: ksud hymo raw delete <src>\n";
                        return 1;
                    }
                    success = HymoFS::delete_rule(cli.args[1]);
                } else if (cmd == "merge") {
                    if (cli.args.size() < 3) {
                        std::cerr << "USAGE: ksud hymo raw merge <src> <target>\n";
                        return 1;
                    }
                    success = HymoFS::add_merge_rule(cli.args[1], cli.args[2]);
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
            } else if (cli.command == "set-mode") {
                if (cli.args.size() < 2) {
                    std::cerr << "USAGE: ksud hymo set-mode <mod_id> <mode>\n";
                    return 1;
                }
                std::string mod_id = cli.args[0];
                std::string mode = cli.args[1];

                auto modes = load_module_modes();
                modes[mod_id] = mode;

                if (save_module_modes(modes)) {
                    std::cout << "Set mode for " << mod_id << " to " << mode << "\n";
                } else {
                    std::cerr << "Failed to save module modes.\n";
                    return 1;
                }
                return 0;
            } else if (cli.command == "add-rule") {
                if (cli.args.size() < 3) {
                    std::cerr << "USAGE: ksud hymo add-rule <mod_id> <path> <mode>\n";
                    return 1;
                }
                std::string mod_id = cli.args[0];
                std::string path = cli.args[1];
                std::string mode = cli.args[2];

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
            } else if (cli.command == "remove-rule") {
                if (cli.args.size() < 2) {
                    std::cerr << "USAGE: ksud hymo remove-rule <mod_id> <path>\n";
                    return 1;
                }
                std::string mod_id = cli.args[0];
                std::string path = cli.args[1];

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
            } else if (cli.command == "reload") {
                Config config = load_config(cli);
                // Re-initialize logger with config verbosity
                Logger::getInstance().init(config.verbose, DAEMON_LOG_FILE);

                if (HymoFS::is_available()) {
                    LOG_INFO("Reloading HymoFS mappings...");

                    // Determine Mirror Path
                    std::string effective_mirror_path = hymo::HYMO_MIRROR_DEV;
                    if (!config.mirror_path.empty()) {
                        effective_mirror_path = config.mirror_path;
                    } else if (!config.tempdir.empty()) {
                        effective_mirror_path = config.tempdir.string();
                    }
                    const fs::path MIRROR_DIR = effective_mirror_path;

                    // 1. Scan modules
                    auto module_list = scan_modules(config.moduledir, config);

                    // 2. Filter active
                    std::vector<Module> active_modules;
                    std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
                    for (const auto& part : config.partitions)
                        all_partitions.push_back(part);

                    for (const auto& mod : module_list) {
                        // Check for hot unmount marker
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

                    // 3. Sync to mirror
                    LOG_INFO("Syncing modules to mirror...");
                    for (const auto& mod : module_list) {
                        fs::path src = config.moduledir / mod.id;
                        fs::path dst = MIRROR_DIR / mod.id;
                        sync_dir(src, dst);
                    }

                    // 4. Update mappings
                    MountPlan plan = generate_plan(config, module_list, MIRROR_DIR);
                    update_hymofs_mappings(config, module_list, MIRROR_DIR, plan);

                    // Apply Stealth Mode
                    if (HymoFS::set_stealth(config.enable_stealth)) {
                        LOG_INFO("Stealth mode set to: " +
                                 std::string(config.enable_stealth ? "true" : "false"));
                    } else {
                        LOG_WARN("Failed to set stealth mode.");
                    }

                    // Apply HymoFS Enable/Disable
                    if (HymoFS::set_enabled(config.hymofs_enabled)) {
                        LOG_INFO("HymoFS enabled set to: " +
                                 std::string(config.hymofs_enabled ? "true" : "false"));
                    } else {
                        LOG_WARN("Failed to set HymoFS enabled state.");
                    }

                    // Fix mount namespace (reorder mnt_id) if stealth is enabled
                    // This ensures that any new mounts created during reload are also
                    // hidden/reordered
                    if (config.enable_stealth) {
                        if (HymoFS::fix_mounts()) {
                            LOG_INFO("Mount namespace fixed (mnt_id reordered) after reload.");
                        } else {
                            LOG_WARN("Failed to fix mount namespace after reload.");
                        }
                    }

                    // 5. Update Runtime State (daemon_state.json)
                    RuntimeState state = load_runtime_state();

                    state.mount_point = MIRROR_DIR.string();
                    state.hymofs_module_ids = plan.hymofs_module_ids;

                    // Recalculate active mounts for HymoFS
                    state.active_mounts.clear();
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

                    state.save();

                    LOG_INFO("Reload complete.");
                } else {
                    LOG_WARN("HymoFS not available, cannot hot reload.");
                }
                return 0;
            } else if (cli.command == "set-uname") {
                if (cli.args.size() < 2) {
                    std::cerr << "USAGE: ksud hymo set-uname <release> <version>\n";
                    std::cerr << "Example: ksud hymo set-uname \"5.15.0-generic\" \"#1 SMP PREEMPT "
                                 "...\"\n";
                    return 1;
                }
                std::string release = cli.args[0];
                std::string version = cli.args[1];

                // Save to config
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

                    // Apply immediately if HymoFS is available
                    if (HymoFS::is_available()) {
                        if (HymoFS::set_uname(release, version)) {
                            std::cout << "Applied uname spoofing to kernel.\n";
                        } else {
                            std::cerr << "Warning: Failed to apply uname to kernel.\n";
                        }
                    }
                } else {
                    std::cerr << "Failed to save config.\n";
                    return 1;
                }
                return 0;
            } else if (cli.command == "set-mirror") {
                if (cli.args.empty()) {
                    std::cerr << "USAGE: ksud hymo set-mirror <path>\n";
                    return 1;
                }
                std::string path = cli.args[0];
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
            } else if (cli.command != "mount") {
                std::cerr << "Unknown command: " << cli.command << "\n";
                print_hymo_help();
                return 1;
            }
        }

        // Load and merge configuration
        Config config = load_config(cli);
        config.merge_with_cli(cli.moduledir, cli.tempdir, cli.mountsource, cli.verbose,
                              cli.partitions);

        // Re-initialize logger with merged config
        Logger::getInstance().init(config.verbose, DAEMON_LOG_FILE);

        // Camouflage process
        if (!camouflage_process("kworker/u9:1")) {
            LOG_WARN("Failed to camouflage process");
        }

        LOG_INFO("Hymo Daemon Starting...");

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

            // Determine Mirror Path
            // Priority: config.mirror_path > config.tempdir > Default
            // (/dev/hymo_mirror)
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

            // Apply HymoFS Enable/Disable
            if (HymoFS::set_enabled(config.hymofs_enabled)) {
                LOG_INFO("HymoFS enabled set to: " +
                         std::string(config.hymofs_enabled ? "true" : "false"));
            } else {
                LOG_WARN("Failed to set HymoFS enabled state.");
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

                // Scan modules from source to know what to copy
                module_list = scan_modules(config.moduledir, config);

                // Filter modules: only copy if they have content for target partitions
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

                // Update module_list to only include active ones for subsequent steps
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
                    // If using ext4 image, we need to fix permissions after sync
                    if (storage.mode == "ext4") {
                        finalize_storage_permissions(storage.mount_point);
                    }

                    mirror_success = true;
                    hymofs_active = true;
                    storage.mount_point = MIRROR_DIR;

                    plan = generate_plan(config, module_list, MIRROR_DIR);

                    // Prepare plan and update mappings
                    segregate_custom_rules(plan, MIRROR_DIR);
                    update_hymofs_mappings(config, module_list, MIRROR_DIR, plan);
                    exec_result = execute_plan(plan, config);

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

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to setup mirror storage: " + std::string(e.what()));
            }

            if (!mirror_success) {
                LOG_WARN("Mirror setup failed. Falling back to Magic Mount.");

                // Fallback to Magic Mount using source directory directly
                storage.mode = "magic_only";
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
                exec_result = execute_plan(plan, config);
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
            perform_sync(module_list, storage.mount_point, config);

            // **FIX 1: Fix permissions after sync**
            if (storage.mode == "ext4") {
                finalize_storage_permissions(storage.mount_point);
            }

            // **Step 4: Generate Plan**
            LOG_INFO("Generating mount plan...");
            plan = generate_plan(config, module_list, storage.mount_point);

            // **Step 5: Execute Plan**
            exec_result = execute_plan(plan, config);
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
namespace hymo {

// Wrapper function for ksud CLI integration
int cmd_hymo(const std::vector<std::string>& args) {
    // Convert args to argc/argv format
    std::vector<const char*> argv_ptrs;
    argv_ptrs.push_back("hymo");  // Program name
    for (const auto& arg : args) {
        argv_ptrs.push_back(arg.c_str());
    }

    int argc = static_cast<int>(argv_ptrs.size());
    char** argv = const_cast<char**>(argv_ptrs.data());

    return hymo_main(argc, argv);
}

}  // namespace hymo
