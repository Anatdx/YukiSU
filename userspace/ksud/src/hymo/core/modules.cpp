// core/modules.cpp - Module description updates implementation
#include "modules.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include "../defs.hpp"
#include "../mount/hymofs.hpp"
#include "../utils.hpp"
#include "inventory.hpp"
#include "json.hpp"  // Changed include

namespace hymo {

static bool has_content(const fs::path& module_path,
                        const std::vector<std::string>& all_partitions) {
    for (const auto& partition : all_partitions) {
        fs::path part_path = module_path / partition;
        if (has_files_recursive(part_path)) {
            return true;
        }
    }
    return false;
}

void update_module_description(bool success, const std::string& storage_mode, bool nuke_active,
                               size_t overlay_count, size_t magic_count, size_t hymofs_count,
                               const std::string& warning_msg, bool hymofs_active) {
    if (!fs::exists(MODULE_PROP_FILE)) {
        LOG_WARN("module.prop not found, skipping update");
        return;
    }

    // ... (rest of function unchanged, just verifying start matches)
    std::ostringstream desc;
    desc << (success ? "😋" : "😭") << " Hymo";
    if (nuke_active) {
        desc << " 🐾";
    }
    desc << " | ";
    desc << "fs: " << storage_mode << " | ";
    desc << "Modules: " << hymofs_count << " HymoFS + " << overlay_count << " Overlay + "
         << magic_count << " Magic";

    if (!warning_msg.empty()) {
        desc << " " << warning_msg;
    }

    std::ifstream infile(MODULE_PROP_FILE);
    std::string content;
    std::string line;
    bool desc_updated = false;
    bool name_updated = false;

    std::string new_name = hymofs_active ? "Hymo - HymoFS Enabled" : "Hymo";

    while (std::getline(infile, line)) {
        if (line.find("description=") == 0) {
            content += "description=" + desc.str() + "\n";
            desc_updated = true;
        } else if (line.find("name=") == 0) {
            content += "name=" + new_name + "\n";
            name_updated = true;
        } else {
            content += line + "\n";
        }
    }
    infile.close();

    if (!desc_updated) {
        content += "description=" + desc.str() + "\n";
    }
    if (!name_updated) {
        content += "name=" + new_name + "\n";
    }

    // Write back
    std::ofstream outfile(MODULE_PROP_FILE);
    outfile << content;
    outfile.close();

    LOG_DEBUG("Updated module description and name");
}

void print_module_list(const Config& config) {
    auto modules = scan_modules(config.moduledir, config);

    // Build complete partition list (builtin + extra)
    std::vector<std::string> all_partitions = BUILTIN_PARTITIONS;
    for (const auto& part : config.partitions) {
        all_partitions.push_back(part);
    }

    // Filter modules with actual content (including extra partitions)
    std::vector<Module> filtered_modules;
    for (const auto& module : modules) {
        if (has_content(module.source_path, all_partitions)) {
            filtered_modules.push_back(module);
        }
    }

    json::Value root = json::Value::object();
    root["count"] = json::Value((int)filtered_modules.size());

    json::Value mods_arr = json::Value::array();

    for (const auto& mod : filtered_modules) {
        std::string strategy = mod.mode;
        if (strategy == "auto") {
            if (HymoFS::is_available())
                strategy = "hymofs";
            else
                strategy = "overlay";
        }

        json::Value m = json::Value::object();
        m["id"] = json::Value(mod.id);
        m["path"] = json::Value(mod.source_path.string());
        m["mode"] = json::Value(mod.mode);
        m["strategy"] = json::Value(strategy);
        m["name"] = json::Value(mod.name);
        m["version"] = json::Value(mod.version);
        m["author"] = json::Value(mod.author);
        m["description"] = json::Value(mod.description);

        json::Value rules_arr = json::Value::array();
        for (const auto& r : mod.rules) {
            json::Value robj = json::Value::object();
            robj["path"] = json::Value(r.path);
            robj["mode"] = json::Value(r.mode);
            rules_arr.push_back(robj);
        }
        m["rules"] = rules_arr;

        mods_arr.push_back(m);
    }

    root["modules"] = mods_arr;
    std::cout << json::dump(root, 2) << "\n";
}

}  // namespace hymo
