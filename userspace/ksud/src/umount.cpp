#include "umount.hpp"
#include "core/ksucalls.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace ksud {

struct UmountEntry {
    std::string path;
    uint32_t flags{};
};

namespace {

std::vector<UmountEntry> load_umount_config() {
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
        const size_t space = line.find(' ');
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

bool save_umount_entries(const std::vector<UmountEntry>& entries) {
    std::ofstream ofs(UMOUNT_CONFIG_PATH);
    if (!ofs)
        return false;

    ofs << "# KernelSU umount configuration\n";
    for (const auto& entry : entries) {
        ofs << entry.path << " " << entry.flags << "\n";
    }

    return true;
}

}  // namespace

int umount_del_entry(const std::string& mnt) {
    // Always try to drop the entry from the kernel list first. Mount points
    // contributed by the meta module (or other early-boot reporters) never
    // hit the config file, so refusing to act on a missing config entry
    // would leave the UI unable to remove them.
    const int kernel_ret = umount_list_del(mnt);
    if (kernel_ret < 0) {
        LOGW("umount_del: kernel del failed for %s (ret=%d)", mnt.c_str(), kernel_ret);
    }

    auto entries = load_umount_config();
    const auto it = std::remove_if(entries.begin(), entries.end(),
                                   [&mnt](const UmountEntry& e) { return e.path == mnt; });
    if (it == entries.end()) {
        LOGD("umount_del: %s not present in config (kernel-only entry)", mnt.c_str());
        return 0;
    }

    entries.erase(it, entries.end());
    if (!save_umount_entries(entries)) {
        LOGE("Failed to save umount config after removing %s", mnt.c_str());
        return 1;
    }

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
        const size_t space = line.find(' ');
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
        const int ret = umount_list_add(entry.path, entry.flags);
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
    const int ret = umount_list_wipe();
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
