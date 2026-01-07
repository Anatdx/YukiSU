// core/modules.cpp - Module description updates implementation
#include "modules.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include "../hymo_defs.hpp"
#include "../hymo_utils.hpp"
#include "../mount/hymofs.hpp"
#include "inventory.hpp"

namespace hymo {

static std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        if (c == '"')
            o << "\\\"";
        else if (c == '\\')
            o << "\\\\";
        else if (c == '\b')
            o << "\\b";
        else if (c == '\f')
            o << "\\f";
        else if (c == '\n')
            o << "\\n";
        else if (c == '\r')
            o << "\\r";
        else if (c == '\t')
            o << "\\t";
        else if ((unsigned char)c < 0x20) {
            char buf[7];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            o << buf;
        } else
            o << c;
    }
    return o.str();
}

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

    std::cout << "{\n";
    std::cout << "  \"count\": " << filtered_modules.size() << ",\n";
    std::cout << "  \"modules\": [\n";

    for (size_t i = 0; i < filtered_modules.size(); ++i) {
        std::string strategy = filtered_modules[i].mode;
        if (strategy == "auto") {
            if (HymoFS::is_available())
                strategy = "hymofs";
            else
                strategy = "overlay";
        }

        std::cout << "    {\n";
        std::cout << "      \"id\": \"" << json_escape(filtered_modules[i].id) << "\",\n";
        std::cout << "      \"path\": \"" << json_escape(filtered_modules[i].source_path.string())
                  << "\",\n";
        std::cout << "      \"mode\": \"" << json_escape(filtered_modules[i].mode) << "\",\n";
        std::cout << "      \"strategy\": \"" << json_escape(strategy) << "\",\n";
        std::cout << "      \"name\": \"" << json_escape(filtered_modules[i].name) << "\",\n";
        std::cout << "      \"version\": \"" << json_escape(filtered_modules[i].version) << "\",\n";
        std::cout << "      \"author\": \"" << json_escape(filtered_modules[i].author) << "\",\n";
        std::cout << "      \"description\": \"" << json_escape(filtered_modules[i].description)
                  << "\",\n";
        std::cout << "      \"rules\": [\n";
        for (size_t j = 0; j < filtered_modules[i].rules.size(); ++j) {
            std::cout << "        {\n";
            std::cout << "          \"path\": \"" << json_escape(filtered_modules[i].rules[j].path)
                      << "\",\n";
            std::cout << "          \"mode\": \"" << json_escape(filtered_modules[i].rules[j].mode)
                      << "\"\n";
            std::cout << "        }";
            if (j < filtered_modules[i].rules.size() - 1)
                std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "      ]\n";
        std::cout << "    }";
        if (i < filtered_modules.size() - 1) {
            std::cout << ",";
        }
        std::cout << "\n";
    }

    std::cout << "  ]\n";
    std::cout << "}\n";
}

}  // namespace hymo
