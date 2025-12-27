#include "profile.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <dirent.h>
#include <unistd.h>
#include <fstream>

namespace ksud {

int profile_get_sepolicy(const std::string& package) {
    std::string path = std::string(PROFILE_SELINUX_DIR) + package;
    auto content = read_file(path);
    if (content) {
        printf("%s", content->c_str());
        return 0;
    }
    printf("No sepolicy profile for %s\n", package.c_str());
    return 1;
}

int profile_set_sepolicy(const std::string& package, const std::string& policy) {
    ensure_dir_exists(PROFILE_SELINUX_DIR);
    std::string path = std::string(PROFILE_SELINUX_DIR) + package;

    if (!write_file(path, policy)) {
        LOGE("Failed to write sepolicy profile");
        return 1;
    }

    return 0;
}

int profile_get_template(const std::string& id) {
    std::string path = std::string(PROFILE_TEMPLATE_DIR) + id;
    auto content = read_file(path);
    if (content) {
        printf("%s", content->c_str());
        return 0;
    }
    printf("Template %s not found\n", id.c_str());
    return 1;
}

int profile_set_template(const std::string& id, const std::string& template_str) {
    ensure_dir_exists(PROFILE_TEMPLATE_DIR);
    std::string path = std::string(PROFILE_TEMPLATE_DIR) + id;

    if (!write_file(path, template_str)) {
        LOGE("Failed to write template");
        return 1;
    }

    return 0;
}

int profile_delete_template(const std::string& id) {
    std::string path = std::string(PROFILE_TEMPLATE_DIR) + id;
    if (unlink(path.c_str()) != 0) {
        LOGE("Failed to delete template %s", id.c_str());
        return 1;
    }
    return 0;
}

int profile_list_templates() {
    DIR* dir = opendir(PROFILE_TEMPLATE_DIR);
    if (!dir) {
        printf("No templates found\n");
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        printf("%s\n", entry->d_name);
    }

    closedir(dir);
    return 0;
}

int apply_profile_sepolies() {
    DIR* dir = opendir(PROFILE_SELINUX_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;

        std::string path = std::string(PROFILE_SELINUX_DIR) + entry->d_name;
        auto content = read_file(path);
        if (!content)
            continue;

        // TODO: Apply sepolicy via ksucalls
        LOGD("Apply sepolicy for %s", entry->d_name);
    }

    closedir(dir);
    return 0;
}

}  // namespace ksud
