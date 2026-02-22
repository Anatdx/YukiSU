// conf/config.cpp - Configuration implementation
#include "config.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include "../core/json.hpp"
#include "../defs.hpp"
#include "../utils.hpp"

namespace hymo {

Config Config::load_default() {
    Config config;
    fs::path default_path = fs::path(BASE_DIR) / "config.json";
    if (fs::exists(default_path)) {
        try {
            return from_file(default_path);
        } catch (...) {
            LOG_WARN("Failed to load default config, using defaults");
        }
    }
    return config;
}

Config Config::from_file(const fs::path& path) {
    Config config;

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file");
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_str = buffer.str();

    try {
        json::Value root = json::parse(json_str);
        if (root.type == json::Type::Object) {
            const auto& o = root.as_object();

            if (o.count("moduledir"))
                config.moduledir = o.at("moduledir").as_string();
            if (o.count("tempdir"))
                config.tempdir = o.at("tempdir").as_string();
            if (o.count("mountsource"))
                config.mountsource = o.at("mountsource").as_string();
            if (o.count("debug"))
                config.debug = o.at("debug").as_bool();
            if (o.count("verbose"))
                config.verbose = o.at("verbose").as_bool();
            if (o.count("fs_type"))
                config.fs_type = filesystem_type_from_string(o.at("fs_type").as_string());
            if (o.count("disable_umount"))
                config.disable_umount = o.at("disable_umount").as_bool();
            if (o.count("enable_nuke"))
                config.enable_nuke = o.at("enable_nuke").as_bool();
            if (o.count("ignore_protocol_mismatch"))
                config.ignore_protocol_mismatch = o.at("ignore_protocol_mismatch").as_bool();
            if (o.count("enable_kernel_debug"))
                config.enable_kernel_debug = o.at("enable_kernel_debug").as_bool();
            if (o.count("enable_stealth"))
                config.enable_stealth = o.at("enable_stealth").as_bool();
            if (o.count("hymofs_enabled"))
                config.hymofs_enabled = o.at("hymofs_enabled").as_bool();
            if (o.count("mirror_path"))
                config.mirror_path = o.at("mirror_path").as_string();
            if (o.count("uname_release"))
                config.uname_release = o.at("uname_release").as_string();
            if (o.count("uname_version"))
                config.uname_version = o.at("uname_version").as_string();
            if (o.count("mount_stage"))
                config.mount_stage = o.at("mount_stage").as_string();

            if (o.count("partitions") && o.at("partitions").type == json::Type::Array) {
                for (const auto& p : o.at("partitions").as_array()) {
                    if (p.type == json::Type::String) {
                        config.partitions.push_back(p.as_string());
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to parse config JSON: " + std::string(e.what()));
    }

    config.module_modes = load_module_modes();
    config.module_rules = load_module_rules();
    return config;
}

bool Config::save_to_file(const fs::path& path) const {
    json::Value root = json::Value::object();

    root["moduledir"] = json::Value(moduledir.string());

    std::string tempdir_value;
    if (tempdir.empty()) {
        tempdir_value = "/data/adb/hymo/img_mnt";
    } else {
        tempdir_value = tempdir.string();
    }
    root["tempdir"] = json::Value(tempdir_value);

    root["mountsource"] = json::Value(mountsource);
    root["debug"] = json::Value(debug);
    root["verbose"] = json::Value(verbose);
    root["fs_type"] = json::Value(filesystem_type_to_string(fs_type));
    root["disable_umount"] = json::Value(disable_umount);
    root["enable_nuke"] = json::Value(enable_nuke);
    root["ignore_protocol_mismatch"] = json::Value(ignore_protocol_mismatch);
    root["enable_kernel_debug"] = json::Value(enable_kernel_debug);
    root["enable_stealth"] = json::Value(enable_stealth);
    root["hymofs_enabled"] = json::Value(hymofs_enabled);
    if (!mirror_path.empty())
        root["mirror_path"] = json::Value(mirror_path);
    if (!uname_release.empty())
        root["uname_release"] = json::Value(uname_release);
    if (!uname_version.empty())
        root["uname_version"] = json::Value(uname_version);
    if (!mount_stage.empty())
        root["mount_stage"] = json::Value(mount_stage);

    if (!partitions.empty()) {
        json::Value parts = json::Value::array();
        for (const auto& p : partitions) {
            parts.push_back(json::Value(p));
        }
        root["partitions"] = parts;
    }

    std::ofstream file(path);
    if (!file.is_open())
        return false;
    file << json::dump(root, 2);
    return true;
}

void Config::merge_with_cli(const fs::path& moduledir_override, const fs::path& tempdir_override,
                            const std::string& mountsource_override, bool verbose_override,
                            const std::vector<std::string>& partitions_override) {
    if (!moduledir_override.empty()) {
        moduledir = moduledir_override;
    }
    if (!tempdir_override.empty()) {
        tempdir = tempdir_override;
    }
    if (!mountsource_override.empty()) {
        mountsource = mountsource_override;
    }
    if (verbose_override) {
        verbose = true;
    }
    if (!partitions_override.empty()) {
        partitions = partitions_override;
    }
}

std::map<std::string, std::string> load_module_modes() {
    std::map<std::string, std::string> modes;

    fs::path mode_file = fs::path(BASE_DIR) / "module_mode.json";
    if (!fs::exists(mode_file))
        return modes;

    std::ifstream file(mode_file);
    std::stringstream buffer;
    buffer << file.rdbuf();

    try {
        auto root = json::parse(buffer.str());
        if (root.type == json::Type::Object) {
            for (const auto& [key, val] : root.as_object()) {
                if (val.type == json::Type::String) {
                    modes[key] = val.as_string();
                }
            }
        }
    } catch (...) {
    }

    return modes;
}

std::map<std::string, std::vector<ModuleRuleConfig>> load_module_rules() {
    std::map<std::string, std::vector<ModuleRuleConfig>> rules;

    fs::path rules_file = fs::path(BASE_DIR) / "module_rules.json";
    if (!fs::exists(rules_file))
        return rules;

    std::ifstream file(rules_file);
    std::stringstream buffer;
    buffer << file.rdbuf();

    try {
        auto root = json::parse(buffer.str());
        if (root.type == json::Type::Object) {
            for (const auto& [mod_id, list] : root.as_object()) {
                if (list.type == json::Type::Array) {
                    for (const auto& rule : list.as_array()) {
                        if (rule.type == json::Type::Object) {
                            const auto& ro = rule.as_object();
                            if (ro.count("path") && ro.count("mode")) {
                                rules[mod_id].push_back(
                                    {ro.at("path").as_string(), ro.at("mode").as_string()});
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {
    }

    return rules;
}

bool save_module_modes(const std::map<std::string, std::string>& modes) {
    fs::path mode_file = fs::path(BASE_DIR) / "module_mode.json";
    json::Value root = json::Value::object();

    for (const auto& [id, mode] : modes) {
        root[id] = json::Value(mode);
    }

    std::ofstream file(mode_file);
    if (!file.is_open())
        return false;
    file << json::dump(root, 2);
    return true;
}

bool save_module_rules(const std::map<std::string, std::vector<ModuleRuleConfig>>& rules) {
    fs::path rules_file = fs::path(BASE_DIR) / "module_rules.json";
    json::Value root = json::Value::object();

    for (const auto& [id, list] : rules) {
        json::Value arr = json::Value::array();
        for (const auto& rule : list) {
            json::Value ro = json::Value::object();
            ro["path"] = json::Value(rule.path);
            ro["mode"] = json::Value(rule.mode);
            arr.push_back(ro);
        }
        root[id] = arr;
    }

    std::ofstream file(rules_file);
    if (!file.is_open())
        return false;
    file << json::dump(root, 2);
    return true;
}

}  // namespace hymo
