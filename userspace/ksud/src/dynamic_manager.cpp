#include "dynamic_manager.hpp"
#include "boot/apk_sign.hpp"
#include "core/json.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ksud {

namespace {

bool parse_size(const std::string& value, uint32_t* out) {
    if (value.empty())
        return false;

    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != '\0' || errno == ERANGE ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    *out = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_size(const json::Value& value, uint32_t* out) {
    if (value.type == json::Type::Number) {
        if (value.n < 0 || value.n > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        *out = static_cast<uint32_t>(value.n);
        return true;
    }

    if (value.type != json::Type::String) {
        return false;
    }
    return parse_size(value.s, out);
}

bool normalize_hash(const std::string& value, char out[65]) {
    if (value.size() != 64) {
        return false;
    }

    for (size_t i = 0; i < value.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!std::isxdigit(c)) {
            return false;
        }
        out[i] = static_cast<char>(std::tolower(c));
    }
    out[64] = '\0';
    return true;
}

bool normalize_hash(const json::Value& value, char out[65]) {
    return value.type == json::Type::String && normalize_hash(value.s, out);
}

bool same_sign(const DynamicManagerSign& lhs, const DynamicManagerSign& rhs) {
    return lhs.size == rhs.size && std::strcmp(lhs.hash, rhs.hash) == 0;
}

bool contains_sign(const std::vector<DynamicManagerSign>& signs, const DynamicManagerSign& sign) {
    return std::any_of(signs.begin(), signs.end(), [&](const DynamicManagerSign& existing) {
        return same_sign(existing, sign);
    });
}

bool append_sign(const json::Value& object, std::vector<DynamicManagerSign>* signs) {
    if (object.type != json::Type::Object) {
        return false;
    }

    const auto size_it = object.o.find("size");
    const auto hash_it = object.o.find("hash");
    if (size_it == object.o.end() || hash_it == object.o.end()) {
        return false;
    }

    DynamicManagerSign sign{};
    if (!parse_size(size_it->second, &sign.size) || !normalize_hash(hash_it->second, sign.hash)) {
        return false;
    }

    if (!contains_sign(*signs, sign)) {
        signs->push_back(sign);
    }
    return true;
}

void append_signs(const json::Value& value, std::vector<DynamicManagerSign>* signs) {
    if (value.type == json::Type::Array) {
        for (const auto& item : value.a) {
            append_sign(item, signs);
        }
        return;
    }

    if (append_sign(value, signs)) {
        return;
    }

    if (value.type != json::Type::Object) {
        return;
    }

    static constexpr const char* kArrayKeys[] = {
        "managers",
        "dynamic_managers",
        "signs",
    };
    for (const char* key : kArrayKeys) {
        const auto it = value.o.find(key);
        if (it != value.o.end()) {
            append_signs(it->second, signs);
        }
    }
}

std::string sign_to_json(const DynamicManagerSign& sign) {
    std::ostringstream ss;
    ss << "  {\n";
    ss << "    \"size\": \"0x" << std::hex << std::nouppercase << sign.size << "\",\n";
    ss << "    \"hash\": \"" << sign.hash << "\"\n";
    ss << "  }";
    return ss.str();
}

std::string signs_to_json(const std::vector<DynamicManagerSign>& signs) {
    if (signs.empty()) {
        return "[]\n";
    }

    std::ostringstream ss;
    ss << "[\n";
    for (size_t i = 0; i < signs.size(); i++) {
        ss << sign_to_json(signs[i]);
        if (i + 1 < signs.size()) {
            ss << ",";
        }
        ss << "\n";
    }
    ss << "]\n";
    return ss.str();
}

bool save_signs(const std::vector<DynamicManagerSign>& signs) {
    if (!ensure_dir_exists(WORKING_DIR)) {
        printf("Failed to create %s\n", WORKING_DIR);
        return false;
    }
    if (!write_file(DYNAMIC_MANAGER_CONFIG_PATH, signs_to_json(signs))) {
        printf("Failed to write %s\n", DYNAMIC_MANAGER_CONFIG_PATH);
        return false;
    }
    chmod(DYNAMIC_MANAGER_CONFIG_PATH, 0600);
    return true;
}

int apply_signs(const std::vector<DynamicManagerSign>& signs) {
    if (signs.size() > KSU_DYNAMIC_MANAGER_MAX_SIGNS) {
        printf("Too many dynamic manager signatures: %zu > %u\n", signs.size(),
               KSU_DYNAMIC_MANAGER_MAX_SIGNS);
        return 1;
    }

    if (set_dynamic_managers(signs) != 0) {
        printf("Failed to send dynamic manager signatures to kernel\n");
        return 1;
    }
    return 0;
}

void print_help() {
    printf("USAGE: ksud dynamic <SUBCOMMAND>\n\n");
    printf("SUBCOMMANDS:\n");
    printf("  get-sign [--json] <APK>   Get APK v2 signature without applying it\n");
    printf("  get-sign [--json] --uid <UID>\n");
    printf("  set-hash <size> <hash>   Persist and apply a size/hash signature\n");
    printf("  set-apk <APK>            Extract APK v2 signature, persist, and apply\n");
    printf("  set-uid <UID>            Extract signature from an installed UID's APK\n");
    printf("  list                     List persisted dynamic manager signatures\n");
    printf("  del <size> <hash>        Delete a persisted size/hash and apply\n");
    printf("  clear                    Clear persisted signatures and apply\n");
    printf("  help                     Show this help\n");
}

const char* bool_json(bool value) {
    return value ? "true" : "false";
}

void print_apk_signature(const ApkSignatureInfo& info, bool json) {
    if (json) {
        printf("{\n");
        printf("  \"v1\": %s,\n", bool_json(info.v1));
        printf("  \"v2\": {\n");
        printf("    \"has\": %s,\n", bool_json(info.v2.has));
        printf("    \"hash\": \"%s\",\n", info.v2.hash.c_str());
        printf("    \"size\": \"0x%x\"\n", info.v2.size);
        printf("  },\n");
        printf("  \"v3\": %s,\n", bool_json(info.v3));
        printf("  \"v3.1\": %s\n", bool_json(info.v31));
        printf("}\n");
        return;
    }

    if (info.v2.has) {
        printf("size: 0x%x\n", info.v2.size);
        printf("hash: %s\n", info.v2.hash.c_str());
    }
    if (info.v1 || info.v3 || info.v31) {
        printf("Warning: Found v1/v3/v3.1 sign data\n");
    }
    if (!info.v2.has) {
        printf("Warning: v2 sign data not found\n");
    }
}

int print_apk_signature(const std::string& apk, bool json) {
    const auto info = get_apk_signature(apk);
    if (!info.valid) {
        printf("Failed to get APK signature: %s\n", apk.c_str());
        return 1;
    }

    print_apk_signature(info, json);
    return info.v2.has ? 0 : 1;
}

bool sign_from_hash_args(const std::string& size_arg, const std::string& hash_arg,
                         DynamicManagerSign* sign) {
    DynamicManagerSign parsed{};
    if (!parse_size(size_arg, &parsed.size)) {
        printf("Invalid size: %s\n", size_arg.c_str());
        return false;
    }
    if (!normalize_hash(hash_arg, parsed.hash)) {
        printf("Invalid hash: %s\n", hash_arg.c_str());
        return false;
    }

    *sign = parsed;
    return true;
}

bool sign_from_apk(const std::string& apk, DynamicManagerSign* sign) {
    const auto info = get_apk_signature(apk);
    if (!info.valid) {
        printf("Failed to get APK signature: %s\n", apk.c_str());
        return false;
    }

    if (info.v1 || info.v3 || info.v31) {
        printf("Warning: Found v1/v3/v3.1 sign data\n");
    }
    if (!info.v2.has) {
        printf("Warning: v2 sign data not found\n");
        return false;
    }

    DynamicManagerSign parsed{};
    parsed.size = info.v2.size;
    if (!normalize_hash(info.v2.hash, parsed.hash)) {
        printf("Invalid v2 hash extracted from APK: %s\n", apk.c_str());
        return false;
    }

    *sign = parsed;
    return true;
}

std::vector<std::string> packages_for_uid(uint32_t uid, uint32_t user_id) {
    std::vector<std::string> packages;
    const std::string user = std::to_string(user_id);
    ExecResult result = exec_command({"cmd", "package", "list", "packages", "--user", user, "-U"});
    if (result.exit_code != 0 || result.stdout_str.empty()) {
        result = exec_command({"pm", "list", "packages", "--user", user, "-U"});
    }
    if (result.exit_code != 0 || result.stdout_str.empty()) {
        result = exec_command({"cmd", "package", "list", "packages", "-U"});
    }
    if (result.exit_code != 0 || result.stdout_str.empty()) {
        result = exec_command({"pm", "list", "packages", "-U"});
    }

    for (const auto& raw_line : split(result.stdout_str, '\n')) {
        const std::string line = trim(raw_line);
        if (line.empty()) {
            continue;
        }

        const size_t pkg_pos = line.find("package:");
        const size_t uid_pos = line.find("uid:");
        if (pkg_pos == std::string::npos || uid_pos == std::string::npos) {
            continue;
        }

        const size_t pkg_start = pkg_pos + strlen("package:");
        size_t pkg_end = line.find_first_of(" \t\r\n", pkg_start);
        if (pkg_end == std::string::npos) {
            pkg_end = line.size();
        }

        uint32_t parsed_uid = 0;
        const size_t uid_start = uid_pos + strlen("uid:");
        size_t uid_end = line.find_first_of(" \t\r\n", uid_start);
        if (uid_end == std::string::npos) {
            uid_end = line.size();
        }
        if (!parse_size(line.substr(uid_start, uid_end - uid_start), &parsed_uid)) {
            continue;
        }

        if (parsed_uid == uid) {
            packages.push_back(line.substr(pkg_start, pkg_end - pkg_start));
        }
    }

    return packages;
}

std::vector<std::string> apk_paths_for_package(const std::string& package_name, uint32_t user_id) {
    std::vector<std::string> paths;
    const std::string user = std::to_string(user_id);
    ExecResult result = exec_command({"cmd", "package", "path", "--user", user, package_name});
    if (result.exit_code != 0 || result.stdout_str.empty()) {
        result = exec_command({"pm", "path", "--user", user, package_name});
    }
    if (result.exit_code != 0 || result.stdout_str.empty()) {
        result = exec_command({"cmd", "package", "path", package_name});
    }
    if (result.exit_code != 0 || result.stdout_str.empty()) {
        result = exec_command({"pm", "path", package_name});
    }

    for (const auto& raw_line : split(result.stdout_str, '\n')) {
        std::string line = trim(raw_line);
        if (!starts_with(line, "package:")) {
            continue;
        }
        line.erase(0, strlen("package:"));
        if (!line.empty()) {
            paths.push_back(line);
        }
    }

    std::stable_sort(paths.begin(), paths.end(),
                     [](const std::string& lhs, const std::string& rhs) {
                         return ends_with(lhs, "/base.apk") && !ends_with(rhs, "/base.apk");
                     });
    return paths;
}

bool sign_from_uid(uint32_t uid, DynamicManagerSign* sign) {
    constexpr uint32_t kPerUserRange = 100000;
    const uint32_t user_id = uid / kPerUserRange;
    const auto packages = packages_for_uid(uid, user_id);
    if (packages.empty()) {
        printf("No package found for uid: %u\n", uid);
        return false;
    }

    for (const auto& package_name : packages) {
        const auto paths = apk_paths_for_package(package_name, user_id);
        for (const auto& path : paths) {
            DynamicManagerSign parsed{};
            if (sign_from_apk(path, &parsed)) {
                printf("package: %s\n", package_name.c_str());
                printf("apk: %s\n", path.c_str());
                *sign = parsed;
                return true;
            }
        }
    }

    printf("No APK with v2 signature found for uid: %u\n", uid);
    return false;
}

bool apk_from_uid(uint32_t uid, std::string* package_out, std::string* apk_out) {
    constexpr uint32_t kPerUserRange = 100000;
    const uint32_t user_id = uid / kPerUserRange;
    const auto packages = packages_for_uid(uid, user_id);
    if (packages.empty()) {
        printf("No package found for uid: %u\n", uid);
        return false;
    }

    for (const auto& package_name : packages) {
        const auto paths = apk_paths_for_package(package_name, user_id);
        for (const auto& path : paths) {
            const auto info = get_apk_signature(path);
            if (info.valid && info.v2.has) {
                *package_out = package_name;
                *apk_out = path;
                return true;
            }
        }
    }

    printf("No APK with v2 signature found for uid: %u\n", uid);
    return false;
}

int add_sign(DynamicManagerSign sign) {
    auto signs = load_dynamic_manager_signs();
    if (!contains_sign(signs, sign)) {
        if (signs.size() >= KSU_DYNAMIC_MANAGER_MAX_SIGNS) {
            printf("Too many dynamic manager signatures: %zu >= %u\n", signs.size(),
                   KSU_DYNAMIC_MANAGER_MAX_SIGNS);
            return 1;
        }
        signs.push_back(sign);
    }

    if (!save_signs(signs)) {
        return 1;
    }
    if (apply_signs(signs) != 0) {
        return 1;
    }

    printf("size: 0x%x\n", sign.size);
    printf("hash: %s\n", sign.hash);
    return 0;
}

}  // namespace

std::vector<DynamicManagerSign> load_dynamic_manager_signs() {
    const auto content = read_file(DYNAMIC_MANAGER_CONFIG_PATH);
    if (!content) {
        return {};
    }

    std::vector<DynamicManagerSign> signs;
    try {
        append_signs(json::parse(*content), &signs);
    } catch (...) {
        LOGW("dynamic_manager: failed to parse %s", DYNAMIC_MANAGER_CONFIG_PATH);
        return {};
    }

    if (signs.size() > KSU_DYNAMIC_MANAGER_MAX_SIGNS) {
        signs.resize(KSU_DYNAMIC_MANAGER_MAX_SIGNS);
    }
    return signs;
}

int load_and_apply_dynamic_managers() {
    const auto signs = load_dynamic_manager_signs();
    if (set_dynamic_managers(signs) != 0) {
        LOGW("dynamic_manager: failed to send %zu signature(s) to kernel", signs.size());
        return 1;
    }

    LOGI("dynamic_manager: loaded %zu signature(s)", signs.size());
    return 0;
}

int cmd_dynamic_manager(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "-h" || args[0] == "--help") {
        print_help();
        return args.empty() ? 1 : 0;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "get-sign") {
        bool json = false;
        bool uid_mode = false;
        std::string target;

        for (size_t i = 1; i < args.size(); i++) {
            if (args[i] == "--json") {
                json = true;
            } else if (args[i] == "--uid" && i + 1 < args.size()) {
                uid_mode = true;
                target = args[++i];
            } else if (target.empty()) {
                target = args[i];
            } else {
                printf("Unexpected argument: %s\n", args[i].c_str());
                return 1;
            }
        }

        if (target.empty()) {
            printf("USAGE: ksud dynamic get-sign [--json] <APK>\n");
            printf("       ksud dynamic get-sign [--json] --uid <UID>\n");
            return 1;
        }

        if (uid_mode) {
            uint32_t uid = 0;
            if (!parse_size(target, &uid)) {
                printf("Invalid uid: %s\n", target.c_str());
                return 1;
            }

            std::string package_name;
            std::string apk;
            if (!apk_from_uid(uid, &package_name, &apk)) {
                return 1;
            }
            if (!json) {
                printf("package: %s\n", package_name.c_str());
                printf("apk: %s\n", apk.c_str());
            }
            return print_apk_signature(apk, json);
        }

        return print_apk_signature(target, json);
    }

    if (subcmd == "set-hash" && args.size() == 3) {
        DynamicManagerSign sign{};
        return sign_from_hash_args(args[1], args[2], &sign) ? add_sign(sign) : 1;
    }

    if (subcmd == "set-apk" && args.size() == 2) {
        DynamicManagerSign sign{};
        return sign_from_apk(args[1], &sign) ? add_sign(sign) : 1;
    }

    if (subcmd == "set-uid" && args.size() == 2) {
        uint32_t uid = 0;
        if (!parse_size(args[1], &uid)) {
            printf("Invalid uid: %s\n", args[1].c_str());
            return 1;
        }

        DynamicManagerSign sign{};
        return sign_from_uid(uid, &sign) ? add_sign(sign) : 1;
    }

    if (subcmd == "list" && args.size() == 1) {
        const auto signs = load_dynamic_manager_signs();
        for (const auto& sign : signs) {
            printf("size: 0x%x\n", sign.size);
            printf("hash: %s\n", sign.hash);
        }
        return 0;
    }

    if (subcmd == "del" && args.size() == 3) {
        DynamicManagerSign target{};
        if (!sign_from_hash_args(args[1], args[2], &target)) {
            return 1;
        }

        auto signs = load_dynamic_manager_signs();
        const auto old_size = signs.size();
        signs.erase(
            std::remove_if(signs.begin(), signs.end(),
                           [&](const DynamicManagerSign& sign) { return same_sign(sign, target); }),
            signs.end());
        if (signs.size() == old_size) {
            printf("Signature not found\n");
            return 1;
        }

        if (!save_signs(signs)) {
            return 1;
        }
        return apply_signs(signs);
    }

    if (subcmd == "clear" && args.size() == 1) {
        const std::vector<DynamicManagerSign> signs;
        if (!save_signs(signs)) {
            return 1;
        }
        return apply_signs(signs);
    }

    printf("Unknown dynamic subcommand: %s\n", subcmd.c_str());
    print_help();
    return 1;
}

}  // namespace ksud
