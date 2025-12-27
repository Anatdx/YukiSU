#include "sepolicy.hpp"
#include "../core/ksucalls.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace ksud {

// Constants matching kernel interface
static constexpr size_t SEPOLICY_MAX_LEN = 128;

static constexpr uint32_t CMD_NORMAL_PERM = 1;
static constexpr uint32_t CMD_XPERM = 2;
static constexpr uint32_t CMD_TYPE_STATE = 3;
static constexpr uint32_t CMD_TYPE = 4;
static constexpr uint32_t CMD_TYPE_ATTR = 5;
static constexpr uint32_t CMD_ATTR = 6;
static constexpr uint32_t CMD_TYPE_TRANSITION = 7;
static constexpr uint32_t CMD_TYPE_CHANGE = 8;
static constexpr uint32_t CMD_GENFSCON = 9;

// Subcmd for CMD_NORMAL_PERM
static constexpr uint32_t SUBCMD_ALLOW = 1;
static constexpr uint32_t SUBCMD_DENY = 2;
static constexpr uint32_t SUBCMD_AUDITALLOW = 3;
static constexpr uint32_t SUBCMD_DONTAUDIT = 4;

// Subcmd for CMD_XPERM
static constexpr uint32_t SUBCMD_ALLOWXPERM = 1;
static constexpr uint32_t SUBCMD_AUDITALLOWXPERM = 2;
static constexpr uint32_t SUBCMD_DONTAUDITXPERM = 3;

// Subcmd for CMD_TYPE_STATE
static constexpr uint32_t SUBCMD_PERMISSIVE = 1;
static constexpr uint32_t SUBCMD_ENFORCING = 2;

// Subcmd for CMD_TYPE_CHANGE
static constexpr uint32_t SUBCMD_TYPE_CHANGE = 1;
static constexpr uint32_t SUBCMD_TYPE_MEMBER = 2;

// FfiPolicy structure - must match kernel expectations
struct FfiPolicy {
    uint32_t cmd;
    uint32_t subcmd;
    const char* sepol1;
    const char* sepol2;
    const char* sepol3;
    const char* sepol4;
    const char* sepol5;
    const char* sepol6;
    const char* sepol7;
};

// PolicyObject - holds a sepolicy string or represents "all" (*)
class PolicyObject {
public:
    enum Type { NONE, ALL, ONE };

    PolicyObject() : type_(NONE) { memset(buf_, 0, sizeof(buf_)); }

    static PolicyObject none() { return PolicyObject(); }

    static PolicyObject all() {
        PolicyObject obj;
        obj.type_ = ALL;
        return obj;
    }

    static PolicyObject from_str(const std::string& s) {
        PolicyObject obj;
        if (s == "*") {
            obj.type_ = ALL;
        } else if (s.length() < SEPOLICY_MAX_LEN) {
            obj.type_ = ONE;
            strncpy(obj.buf_, s.c_str(), SEPOLICY_MAX_LEN - 1);
            obj.buf_[SEPOLICY_MAX_LEN - 1] = '\0';
        }
        return obj;
    }

    const char* c_ptr() const {
        if (type_ == ONE) {
            return buf_;
        }
        return nullptr;  // NULL for NONE and ALL
    }

    Type type() const { return type_; }

private:
    Type type_;
    char buf_[SEPOLICY_MAX_LEN];
};

// AtomicStatement - a single sepolicy operation to send to kernel
struct AtomicStatement {
    uint32_t cmd;
    uint32_t subcmd;
    PolicyObject sepol1;
    PolicyObject sepol2;
    PolicyObject sepol3;
    PolicyObject sepol4;
    PolicyObject sepol5;
    PolicyObject sepol6;
    PolicyObject sepol7;

    FfiPolicy to_ffi() const {
        return FfiPolicy{cmd,
                         subcmd,
                         sepol1.c_ptr(),
                         sepol2.c_ptr(),
                         sepol3.c_ptr(),
                         sepol4.c_ptr(),
                         sepol5.c_ptr(),
                         sepol6.c_ptr(),
                         sepol7.c_ptr()};
    }
};

// Helper: check if char is valid in sepolicy identifier
static bool is_sepolicy_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

// Helper: skip whitespace
static const char* skip_space(const char* p) {
    while (*p && std::isspace(static_cast<unsigned char>(*p)))
        p++;
    return p;
}

// Helper: parse a single word
static const char* parse_word(const char* p, std::string& out) {
    out.clear();
    while (*p && is_sepolicy_char(*p)) {
        out += *p++;
    }
    return p;
}

// Helper: parse objects (single word, {word1 word2 ...}, or *)
static const char* parse_seobj(const char* p, std::vector<std::string>& out) {
    out.clear();
    p = skip_space(p);

    if (*p == '*') {
        out.push_back("*");
        return p + 1;
    }

    if (*p == '{') {
        p++;  // skip '{'
        while (*p && *p != '}') {
            p = skip_space(p);
            if (*p == '}')
                break;
            std::string word;
            p = parse_word(p, word);
            if (!word.empty()) {
                out.push_back(word);
            }
            p = skip_space(p);
        }
        if (*p == '}')
            p++;
        return p;
    }

    // Single word
    std::string word;
    p = parse_word(p, word);
    if (!word.empty()) {
        out.push_back(word);
    }
    return p;
}

// Parse and expand a single rule into AtomicStatements
static bool parse_rule(const std::string& rule, std::vector<AtomicStatement>& statements) {
    const char* p = rule.c_str();
    p = skip_space(p);

    if (*p == '\0' || *p == '#') {
        return true;  // Empty or comment
    }

    std::string cmd_str;
    p = parse_word(p, cmd_str);

    // allow/deny/auditallow/dontaudit source target:class perm
    if (cmd_str == "allow" || cmd_str == "deny" || cmd_str == "auditallow" ||
        cmd_str == "dontaudit") {
        uint32_t subcmd = 0;
        if (cmd_str == "allow")
            subcmd = SUBCMD_ALLOW;
        else if (cmd_str == "deny")
            subcmd = SUBCMD_DENY;
        else if (cmd_str == "auditallow")
            subcmd = SUBCMD_AUDITALLOW;
        else if (cmd_str == "dontaudit")
            subcmd = SUBCMD_DONTAUDIT;

        std::vector<std::string> sources, targets, classes, perms;

        p = parse_seobj(p, sources);
        p = parse_seobj(p, targets);

        // Parse class (may be target:class format or separate)
        p = skip_space(p);
        if (*p == ':') {
            p++;
            p = parse_seobj(p, classes);
        } else {
            // Check if last target contains ':'
            if (!targets.empty()) {
                std::string& last = targets.back();
                size_t colon = last.find(':');
                if (colon != std::string::npos) {
                    classes.push_back(last.substr(colon + 1));
                    last = last.substr(0, colon);
                } else {
                    p = parse_seobj(p, classes);
                }
            }
        }

        p = parse_seobj(p, perms);

        // Expand to atomic statements
        for (const auto& s : sources) {
            for (const auto& t : targets) {
                for (const auto& c : classes) {
                    for (const auto& perm : perms) {
                        AtomicStatement stmt;
                        stmt.cmd = CMD_NORMAL_PERM;
                        stmt.subcmd = subcmd;
                        stmt.sepol1 = PolicyObject::from_str(s);
                        stmt.sepol2 = PolicyObject::from_str(t);
                        stmt.sepol3 = PolicyObject::from_str(c);
                        stmt.sepol4 = PolicyObject::from_str(perm);
                        statements.push_back(stmt);
                    }
                }
            }
        }
        return true;
    }

    // allowxperm/auditallowxperm/dontauditxperm source target:class operation xperm_set
    if (cmd_str == "allowxperm" || cmd_str == "auditallowxperm" || cmd_str == "dontauditxperm") {
        uint32_t subcmd = 0;
        if (cmd_str == "allowxperm")
            subcmd = SUBCMD_ALLOWXPERM;
        else if (cmd_str == "auditallowxperm")
            subcmd = SUBCMD_AUDITALLOWXPERM;
        else if (cmd_str == "dontauditxperm")
            subcmd = SUBCMD_DONTAUDITXPERM;

        std::vector<std::string> sources, targets, classes;
        std::string operation, perm_set;

        p = parse_seobj(p, sources);
        p = parse_seobj(p, targets);

        p = skip_space(p);
        if (*p == ':') {
            p++;
            p = parse_seobj(p, classes);
        } else if (!targets.empty()) {
            std::string& last = targets.back();
            size_t colon = last.find(':');
            if (colon != std::string::npos) {
                classes.push_back(last.substr(colon + 1));
                last = last.substr(0, colon);
            } else {
                p = parse_seobj(p, classes);
            }
        }

        p = skip_space(p);
        p = parse_word(p, operation);

        // Parse xperm_set (could be { 0x1234 } or just value)
        p = skip_space(p);
        if (*p == '{') {
            const char* start = p;
            while (*p && *p != '}')
                p++;
            if (*p == '}')
                p++;
            perm_set = std::string(start, p);
        } else {
            p = parse_word(p, perm_set);
        }

        for (const auto& s : sources) {
            for (const auto& t : targets) {
                for (const auto& c : classes) {
                    AtomicStatement stmt;
                    stmt.cmd = CMD_XPERM;
                    stmt.subcmd = subcmd;
                    stmt.sepol1 = PolicyObject::from_str(s);
                    stmt.sepol2 = PolicyObject::from_str(t);
                    stmt.sepol3 = PolicyObject::from_str(c);
                    stmt.sepol4 = PolicyObject::from_str(operation);
                    stmt.sepol5 = PolicyObject::from_str(perm_set);
                    statements.push_back(stmt);
                }
            }
        }
        return true;
    }

    // permissive/enforce type
    if (cmd_str == "permissive" || cmd_str == "enforce") {
        uint32_t subcmd = (cmd_str == "permissive") ? SUBCMD_PERMISSIVE : SUBCMD_ENFORCING;

        std::vector<std::string> types;
        p = parse_seobj(p, types);

        for (const auto& t : types) {
            AtomicStatement stmt;
            stmt.cmd = CMD_TYPE_STATE;
            stmt.subcmd = subcmd;
            stmt.sepol1 = PolicyObject::from_str(t);
            statements.push_back(stmt);
        }
        return true;
    }

    // type type_name attr1 attr2 ...
    if (cmd_str == "type") {
        std::string type_name;
        p = skip_space(p);
        p = parse_word(p, type_name);

        std::vector<std::string> attrs;
        p = parse_seobj(p, attrs);

        if (attrs.empty()) {
            // Type with no attributes
            AtomicStatement stmt;
            stmt.cmd = CMD_TYPE;
            stmt.subcmd = 0;
            stmt.sepol1 = PolicyObject::from_str(type_name);
            statements.push_back(stmt);
        } else {
            for (const auto& attr : attrs) {
                AtomicStatement stmt;
                stmt.cmd = CMD_TYPE;
                stmt.subcmd = 0;
                stmt.sepol1 = PolicyObject::from_str(type_name);
                stmt.sepol2 = PolicyObject::from_str(attr);
                statements.push_back(stmt);
            }
        }
        return true;
    }

    // typeattribute type attr1 attr2 ...
    if (cmd_str == "typeattribute") {
        std::vector<std::string> types, attrs;
        p = parse_seobj(p, types);
        p = parse_seobj(p, attrs);

        for (const auto& t : types) {
            for (const auto& attr : attrs) {
                AtomicStatement stmt;
                stmt.cmd = CMD_TYPE_ATTR;
                stmt.subcmd = 0;
                stmt.sepol1 = PolicyObject::from_str(t);
                stmt.sepol2 = PolicyObject::from_str(attr);
                statements.push_back(stmt);
            }
        }
        return true;
    }

    // attribute attr_name
    if (cmd_str == "attribute") {
        std::string attr_name;
        p = skip_space(p);
        p = parse_word(p, attr_name);

        AtomicStatement stmt;
        stmt.cmd = CMD_ATTR;
        stmt.subcmd = 0;
        stmt.sepol1 = PolicyObject::from_str(attr_name);
        statements.push_back(stmt);
        return true;
    }

    // type_transition source target:class default_type [object_name]
    if (cmd_str == "type_transition") {
        std::string source, target, tclass, default_type, object_name;

        p = skip_space(p);
        p = parse_word(p, source);
        p = skip_space(p);
        p = parse_word(p, target);

        // Handle target:class format
        size_t colon = target.find(':');
        if (colon != std::string::npos) {
            tclass = target.substr(colon + 1);
            target = target.substr(0, colon);
        } else {
            p = skip_space(p);
            if (*p == ':') {
                p++;
                p = parse_word(p, tclass);
            } else {
                p = parse_word(p, tclass);
            }
        }

        p = skip_space(p);
        p = parse_word(p, default_type);

        p = skip_space(p);
        if (*p) {
            // Optional object_name (may be quoted)
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    object_name += *p++;
                }
                if (*p == '"')
                    p++;
            } else {
                p = parse_word(p, object_name);
            }
        }

        AtomicStatement stmt;
        stmt.cmd = CMD_TYPE_TRANSITION;
        stmt.subcmd = 0;
        stmt.sepol1 = PolicyObject::from_str(source);
        stmt.sepol2 = PolicyObject::from_str(target);
        stmt.sepol3 = PolicyObject::from_str(tclass);
        stmt.sepol4 = PolicyObject::from_str(default_type);
        if (!object_name.empty()) {
            stmt.sepol5 = PolicyObject::from_str(object_name);
        }
        statements.push_back(stmt);
        return true;
    }

    // type_change/type_member source target:class default_type
    if (cmd_str == "type_change" || cmd_str == "type_member") {
        uint32_t subcmd = (cmd_str == "type_change") ? SUBCMD_TYPE_CHANGE : SUBCMD_TYPE_MEMBER;

        std::string source, target, tclass, default_type;

        p = skip_space(p);
        p = parse_word(p, source);
        p = skip_space(p);
        p = parse_word(p, target);

        size_t colon = target.find(':');
        if (colon != std::string::npos) {
            tclass = target.substr(colon + 1);
            target = target.substr(0, colon);
        } else {
            p = skip_space(p);
            if (*p == ':') {
                p++;
                p = parse_word(p, tclass);
            } else {
                p = parse_word(p, tclass);
            }
        }

        p = skip_space(p);
        p = parse_word(p, default_type);

        AtomicStatement stmt;
        stmt.cmd = CMD_TYPE_CHANGE;
        stmt.subcmd = subcmd;
        stmt.sepol1 = PolicyObject::from_str(source);
        stmt.sepol2 = PolicyObject::from_str(target);
        stmt.sepol3 = PolicyObject::from_str(tclass);
        stmt.sepol4 = PolicyObject::from_str(default_type);
        statements.push_back(stmt);
        return true;
    }

    // genfscon fs_name partial_path fs_context
    if (cmd_str == "genfscon") {
        std::string fs_name, partial_path, fs_context;

        p = skip_space(p);
        p = parse_word(p, fs_name);
        p = skip_space(p);

        // partial_path might be quoted or not
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                partial_path += *p++;
            }
            if (*p == '"')
                p++;
        } else {
            p = parse_word(p, partial_path);
        }

        p = skip_space(p);
        p = parse_word(p, fs_context);

        AtomicStatement stmt;
        stmt.cmd = CMD_GENFSCON;
        stmt.subcmd = 0;
        stmt.sepol1 = PolicyObject::from_str(fs_name);
        stmt.sepol2 = PolicyObject::from_str(partial_path);
        stmt.sepol3 = PolicyObject::from_str(fs_context);
        statements.push_back(stmt);
        return true;
    }

    LOGW("Unknown sepolicy command: %s", cmd_str.c_str());
    return false;
}

// Apply a single atomic statement to kernel
static int apply_statement(const AtomicStatement& stmt) {
    FfiPolicy ffi = stmt.to_ffi();

    SetSepolicyCmd cmd;
    cmd.cmd = 0;
    cmd.arg = reinterpret_cast<uint64_t>(&ffi);

    int ret = set_sepolicy(cmd);
    if (ret < 0) {
        LOGW("Failed to apply sepolicy: cmd=%u subcmd=%u", ffi.cmd, ffi.subcmd);
    }
    return ret;
}

int sepolicy_live_patch(const std::string& policy) {
    std::vector<AtomicStatement> statements;
    int errors = 0;

    // Split by newline and semicolon
    std::istringstream iss(policy);
    std::string line;

    while (std::getline(iss, line)) {
        // Handle semicolon-separated rules
        std::istringstream line_iss(line);
        std::string rule;
        while (std::getline(line_iss, rule, ';')) {
            std::string trimmed = trim(rule);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }

            std::vector<AtomicStatement> rule_stmts;
            if (!parse_rule(trimmed, rule_stmts)) {
                LOGW("Failed to parse rule: %s", trimmed.c_str());
                errors++;
                continue;
            }

            for (const auto& stmt : rule_stmts) {
                if (apply_statement(stmt) < 0) {
                    errors++;
                }
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

static bool is_valid_rule_type(const std::string& trimmed) {
    return starts_with(trimmed, "allow") || starts_with(trimmed, "deny") ||
           starts_with(trimmed, "auditallow") || starts_with(trimmed, "dontaudit") ||
           starts_with(trimmed, "allowxperm") || starts_with(trimmed, "auditallowxperm") ||
           starts_with(trimmed, "dontauditxperm") || starts_with(trimmed, "type ") ||
           starts_with(trimmed, "attribute") || starts_with(trimmed, "permissive") ||
           starts_with(trimmed, "enforce") || starts_with(trimmed, "typeattribute") ||
           starts_with(trimmed, "type_transition") || starts_with(trimmed, "type_change") ||
           starts_with(trimmed, "type_member") || starts_with(trimmed, "genfscon");
}

int sepolicy_check_rule(const std::string& policy_or_file) {
    // Check if it's a file path
    struct stat st;
    if (stat(policy_or_file.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        auto content = read_file(policy_or_file);
        if (!content) {
            printf("Failed to read file: %s\n", policy_or_file.c_str());
            return 1;
        }

        std::istringstream iss(*content);
        std::string line;
        int line_num = 0;
        int errors = 0;

        while (std::getline(iss, line)) {
            line_num++;
            std::string trimmed = trim(line);

            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }

            if (!is_valid_rule_type(trimmed)) {
                printf("Line %d: Unknown rule type: %s\n", line_num, trimmed.c_str());
                errors++;
            }
        }

        if (errors > 0) {
            printf("Found %d invalid rules\n", errors);
            return 1;
        }

        printf("All sepolicy rules are valid\n");
        return 0;
    }

    // Treat as a single rule
    std::string trimmed = trim(policy_or_file);

    if (trimmed.empty()) {
        printf("Invalid: empty rule\n");
        return 1;
    }

    if (is_valid_rule_type(trimmed)) {
        printf("Valid sepolicy rule\n");
        return 0;
    }

    printf("Unknown rule type\n");
    return 1;
}

}  // namespace ksud
