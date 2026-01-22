// core/user_rules.hpp - User-defined HymoFS rules management
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hymo {

// User-defined hide rule
struct UserHideRule {
    std::string path;
};

// Load user-defined hide rules from storage
std::vector<UserHideRule> load_user_hide_rules();

// Save user-defined hide rules to storage
bool save_user_hide_rules(const std::vector<UserHideRule>& rules);

// Add a new user hide rule (and apply it to kernel)
bool add_user_hide_rule(const std::string& path);

// Remove a user hide rule (from storage and kernel)
bool remove_user_hide_rule(const std::string& path);

// List all user-defined hide rules
void list_user_hide_rules();

// Apply all user hide rules to kernel (called during mount)
void apply_user_hide_rules();

}  // namespace hymo
