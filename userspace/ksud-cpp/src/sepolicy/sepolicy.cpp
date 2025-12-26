#include "sepolicy.hpp"
#include "../core/ksucalls.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

namespace ksud {

// SEpolicy command types
enum SepolicyCmd {
    SEPOLICY_CMD_ALLOW = 1,
    SEPOLICY_CMD_DENY = 2,
    SEPOLICY_CMD_AUDITALLOW = 3,
    SEPOLICY_CMD_DONTAUDIT = 4,
    SEPOLICY_CMD_TYPE = 5,
    SEPOLICY_CMD_ATTRIBUTE = 6,
    SEPOLICY_CMD_PERMISSIVE = 7,
    SEPOLICY_CMD_ENFORCE = 8,
    SEPOLICY_CMD_TYPEATTRIBUTE = 9,
    SEPOLICY_CMD_TYPE_TRANSITION = 10,
    SEPOLICY_CMD_TYPE_CHANGE = 11,
    SEPOLICY_CMD_TYPE_MEMBER = 12,
    SEPOLICY_CMD_GENFSCON = 13,
};

static int parse_and_apply_rule(const std::string& rule) {
    std::string trimmed = trim(rule);
    if (trimmed.empty() || trimmed[0] == '#') {
        return 0; // Skip empty lines and comments
    }

    // Parse rule format: "allow source target:class { perms }"
    // This is a simplified parser - the kernel does the actual parsing

    SetSepolicyCmd cmd;
    cmd.cmd = reinterpret_cast<uint64_t>(trimmed.c_str());
    cmd.arg = trimmed.length();

    int ret = set_sepolicy(cmd);
    if (ret < 0) {
        LOGE("Failed to apply sepolicy rule: %s", trimmed.c_str());
        return -1;
    }

    return 0;
}

int sepolicy_live_patch(const std::string& policy) {
    // Split by newline or semicolon
    std::istringstream iss(policy);
    std::string line;
    int errors = 0;

    while (std::getline(iss, line)) {
        // Also handle semicolon-separated rules
        std::istringstream line_iss(line);
        std::string rule;
        while (std::getline(line_iss, rule, ';')) {
            if (parse_and_apply_rule(rule) != 0) {
                errors++;
            }
        }
    }

    return errors > 0 ? 1 : 0;
}

int sepolicy_apply_file(const std::string& file) {
    auto content = read_file(file);
    if (!content) {
        LOGE("Failed to read file: %s", file.c_str());
        return 1;
    }

    return sepolicy_live_patch(*content);
}

int sepolicy_check_rule(const std::string& policy) {
    // Try to parse the rule without applying
    std::string trimmed = trim(policy);

    // Basic validation
    if (trimmed.empty()) {
        printf("Invalid: empty rule\n");
        return 1;
    }

    // Check for valid rule types
    if (starts_with(trimmed, "allow") ||
        starts_with(trimmed, "deny") ||
        starts_with(trimmed, "auditallow") ||
        starts_with(trimmed, "dontaudit") ||
        starts_with(trimmed, "type ") ||
        starts_with(trimmed, "attribute") ||
        starts_with(trimmed, "permissive") ||
        starts_with(trimmed, "enforce") ||
        starts_with(trimmed, "typeattribute") ||
        starts_with(trimmed, "type_transition") ||
        starts_with(trimmed, "type_change") ||
        starts_with(trimmed, "type_member") ||
        starts_with(trimmed, "genfscon")) {
        printf("Valid sepolicy rule\n");
        return 0;
    }

    printf("Unknown rule type\n");
    return 1;
}

} // namespace ksud
