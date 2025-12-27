#include "umount.hpp"
#include "core/ksucalls.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <unistd.h>
#include <fstream>
#include <sstream>
#include <vector>

namespace ksud {

struct UmountEntry {
    std::string path;
    uint32_t flags;
};

static std::vector<UmountEntry> load_umount_config() {
    std::vector<UmountEntry> entries;
    auto content = read_file(UMOUNT_CONFIG_PATH);
    if (!content)
        return entries;

    std::istringstream iss(*content);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        UmountEntry entry;
        size_t space = line.find(' ');
        if (space != std::string::npos) {
            entry.path = line.substr(0, space);
            entry.flags = std::stoul(line.substr(space + 1));
        } else {
            entry.path = line;
            entry.flags = 0;
        }
        entries.push_back(entry);
    }

    return entries;
}

static bool save_umount_entries(const std::vector<UmountEntry>& entries) {
    std::ofstream ofs(UMOUNT_CONFIG_PATH);
    if (!ofs)
        return false;

    ofs << "# KernelSU umount configuration\n";
    for (const auto& entry : entries) {
        ofs << entry.path << " " << entry.flags << "\n";
    }

    return true;
}

int umount_remove_entry(const std::string& mnt) {
    auto entries = load_umount_config();

    auto it = std::remove_if(entries.begin(), entries.end(),
                             [&mnt](const UmountEntry& e) { return e.path == mnt; });

    if (it == entries.end()) {
        printf("Mount point %s not found in config\n", mnt.c_str());
        return 1;
    }

    entries.erase(it, entries.end());

    if (!save_umount_entries(entries)) {
        LOGE("Failed to save umount config");
        return 1;
    }

    // Also remove from kernel
    umount_list_del(mnt);

    return 0;
}

int umount_save_config() {
    auto list = umount_list_list();
    if (!list) {
        LOGE("Failed to get umount list from kernel");
        return 1;
    }

    std::vector<UmountEntry> entries;
    std::istringstream iss(*list);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty())
            continue;

        UmountEntry entry;
        size_t space = line.find(' ');
        if (space != std::string::npos) {
            entry.path = line.substr(0, space);
            entry.flags = std::stoul(line.substr(space + 1));
        } else {
            entry.path = line;
            entry.flags = 0;
        }
        entries.push_back(entry);
    }

    if (!save_umount_entries(entries)) {
        LOGE("Failed to save umount config");
        return 1;
    }

    LOGI("Saved umount config with %zu entries", entries.size());
    return 0;
}

int umount_apply_config() {
    auto entries = load_umount_config();

    for (const auto& entry : entries) {
        int ret = umount_list_add(entry.path, entry.flags);
        if (ret < 0) {
            LOGW("Failed to add %s to umount list", entry.path.c_str());
        } else {
            LOGD("Added %s to umount list (flags=%u)", entry.path.c_str(), entry.flags);
        }
    }

    LOGI("Applied %zu umount entries", entries.size());
    return 0;
}

int umount_clear_config() {
    // Clear kernel list
    int ret = umount_list_wipe();
    if (ret < 0) {
        LOGE("Failed to clear kernel umount list");
        return 1;
    }

    // Clear config file
    unlink(UMOUNT_CONFIG_PATH);

    LOGI("Cleared umount configuration");
    return 0;
}

}  // namespace ksud
