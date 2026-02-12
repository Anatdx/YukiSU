// core/user_rules.cpp - User-defined HymoFS rules management
#include "user_rules.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include "../mount/hymofs.hpp"
#include "../utils.hpp"
#include "json.hpp"

#define USER_HIDE_RULES_FILE "/data/adb/hymo/user_hide_rules.json"

namespace hymo {

std::vector<UserHideRule> load_user_hide_rules() {
    std::vector<UserHideRule> rules;
    std::ifstream file(USER_HIDE_RULES_FILE);

    if (!file.is_open()) {
        return rules;  // File doesn't exist yet, return empty
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();

    try {
        auto root = json::parse(buffer.str());
        if (root.type == json::Type::Array) {
            for (const auto& val : root.as_array()) {
                if (val.type == json::Type::String) {
                    rules.push_back({val.as_string()});
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse user rules JSON: " + std::string(e.what()));
    }

    LOG_INFO("Loaded " + std::to_string(rules.size()) + " user hide rules");
    return rules;
}

bool save_user_hide_rules(const std::vector<UserHideRule>& rules) {
    // Ensure directory exists
    fs::path file_path(USER_HIDE_RULES_FILE);
    fs::path dir = file_path.parent_path();

    try {
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create directory: " + std::string(e.what()));
        return false;
    }

    std::ofstream file(USER_HIDE_RULES_FILE);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open user hide rules file for writing");
        return false;
    }

    json::Value root = json::Value::array();
    for (const auto& rule : rules) {
        root.push_back(json::Value(rule.path));
    }

    file << json::dump(root, 2);
    file.close();
    LOG_INFO("Saved " + std::to_string(rules.size()) + " user hide rules");
    return true;
}

bool add_user_hide_rule(const std::string& path) {
    // Validate path
    if (path.empty() || path[0] != '/') {
        std::cerr << "Error: Path must be absolute (start with /)\n";
        return false;
    }

    // Load existing rules
    auto rules = load_user_hide_rules();

    // Check if rule already exists
    for (const auto& rule : rules) {
        if (rule.path == path) {
            std::cout << "Hide rule already exists: " << path << "\n";
            return true;
        }
    }

    // Add new rule
    rules.push_back({path});

    // Save to file
    if (!save_user_hide_rules(rules)) {
        std::cerr << "Error: Failed to save user hide rules\n";
        return false;
    }

    // Apply to kernel immediately if HymoFS is available
    if (HymoFS::is_available()) {
        if (!HymoFS::hide_path(path)) {
            std::cerr << "Warning: Failed to apply hide rule to kernel (saved to file)\n";
            // We still return true because it was saved
        } else {
            std::cout << "Hide rule added and applied: " << path << "\n";
        }
    } else {
        std::cout << "Hide rule added (will be applied on next boot): " << path << "\n";
    }

    LOG_INFO("Added user hide rule: " + path);
    return true;
}

bool remove_user_hide_rule(const std::string& path) {
    auto rules = load_user_hide_rules();

    // Find and remove the rule
    auto it = std::remove_if(rules.begin(), rules.end(),
                             [&path](const UserHideRule& r) { return r.path == path; });

    if (it == rules.end()) {
        std::cerr << "Error: Hide rule not found: " << path << "\n";
        return false;
    }

    rules.erase(it, rules.end());

    // Save updated rules
    if (!save_user_hide_rules(rules)) {
        std::cerr << "Error: Failed to save user hide rules\n";
        return false;
    }

    // Try to remove from kernel (but we can't actually do this safely
    // because kernel doesn't distinguish user vs module rules)
    std::cout << "Hide rule removed from user list: " << path << "\n";
    std::cout << "Note: Kernel rule will persist until next reload\n";

    LOG_INFO("Removed user hide rule: " + path);
    return true;
}

void list_user_hide_rules() {
    auto rules = load_user_hide_rules();

    json::Value root = json::Value::array();
    for (const auto& rule : rules) {
        root.push_back(json::Value(rule.path));
    }
    std::cout << json::dump(root, 2) << "\n";
}

void apply_user_hide_rules() {
    auto rules = load_user_hide_rules();

    if (rules.empty()) {
        LOG_INFO("No user hide rules to apply");
        return;
    }

    int success = 0;
    int failed = 0;

    if (!HymoFS::is_available()) {
        LOG_WARN("HymoFS not available, cannot apply user hide rules");
        return;
    }

    for (const auto& rule : rules) {
        if (HymoFS::hide_path(rule.path)) {
            success++;
        } else {
            failed++;
            LOG_WARN("Failed to apply user hide rule: " + rule.path);
        }
    }

    LOG_INFO("Applied user hide rules: " + std::to_string(success) + " success, " +
             std::to_string(failed) + " failed");
}

}  // namespace hymo
