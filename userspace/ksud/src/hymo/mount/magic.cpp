// mount/magic.cpp - Magic mount implementation
#include "magic.hpp"
#include <fcntl.h>
#include <limits.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include "../core/state.hpp"
#include "../defs.hpp"
#include "../utils.hpp"
#include "mount_utils.hpp"
#include "partition_utils.hpp"

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif // #ifndef TMPFS_MAGIC

namespace hymo {

struct MountStats {
    int total_mounts = 0;
    int successful_mounts = 0;
    int failed_mounts = 0;
    int tmpfs_created = 0;
    int files_mounted = 0;
    int dirs_mounted = 0;
    int symlinks_created = 0;
    int overlayfs_mounts = 0;
};

static MountStats g_mount_stats;

enum class NodeFileType { RegularFile, Directory, Symlink, Whiteout };

struct Node {
    std::string name;
    NodeFileType file_type;
    std::unordered_map<std::string, Node> children;
    fs::path module_path;     // Path to module file
    std::string module_name;  // Module ID that owns this node
    bool replace = false;     // Directory marked for replacement (xattr/file)
    bool skip = false;        // Skip mounting this node
    bool done = false;        // Already processed flag
};

static bool dir_is_replace(const fs::path& path) {
    char buf[4];
    ssize_t len = lgetxattr(path.c_str(), REPLACE_DIR_XATTR, buf, sizeof(buf));
    if (len > 0 && buf[0] == 'y') {
        return true;
    }

    if (fs::exists(path / REPLACE_DIR_FILE_NAME)) {
        return true;
    }

    return false;
}

static NodeFileType get_file_type(const fs::path& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return NodeFileType::RegularFile;
    }

    if (S_ISCHR(st.st_mode) && st.st_rdev == 0) {
        return NodeFileType::Whiteout;
    } else if (S_ISDIR(st.st_mode)) {
        return NodeFileType::Directory;
    } else if (S_ISLNK(st.st_mode)) {
        return NodeFileType::Symlink;
    } else {
        return NodeFileType::RegularFile;
    }
}

static bool collect_module_files(Node& node, const fs::path& module_dir,
                                 const std::string& module_name = "") {
    if (!fs::exists(module_dir)) {
        LOG_DEBUG("Module dir does not exist: " + module_dir.string());
        return false;
    }

    if (!fs::is_directory(module_dir)) {
        LOG_DEBUG("Module dir is not a directory: " + module_dir.string());
        return false;
    }

    bool has_file = false;
    int file_count = 0;
    int dir_count = 0;

    try {
        for (const auto& entry : fs::directory_iterator(module_dir)) {
            std::string name = entry.path().filename().string();
            NodeFileType ft = get_file_type(entry.path());

            auto it = node.children.find(name);
            Node* child = nullptr;

            if (it != node.children.end()) {
                // Node already exists from another module - merge
                child = &it->second;
            } else {
                // Create new node
                Node new_child;
                new_child.name = name;
                new_child.file_type = ft;
                new_child.module_path = entry.path();
                new_child.module_name = module_name;
                node.children[name] = new_child;
                child = &node.children[name];
            }

            if (ft == NodeFileType::Directory) {
                dir_count++;
                child->replace = dir_is_replace(entry.path());
                bool child_has_file = collect_module_files(*child, entry.path(), module_name);
                has_file |= child_has_file || child->replace;
                if (child->replace) {
                    LOG_DEBUG("  Replace dir: " + entry.path().string());
                }
            } else {
                file_count++;
                has_file = true;
            }
        }

        if (has_file) {
            LOG_DEBUG("Scanned " + module_dir.string() + ": " + std::to_string(file_count) +
                      " files, " + std::to_string(dir_count) + " dirs");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception scanning " + module_dir.string() + ": " + std::string(e.what()));
        return false;
    }

    return has_file;
}

static Node* collect_all_modules(const std::vector<fs::path>& module_paths,
                                 const std::vector<std::string>& extra_partitions) {
    Node* root = new Node{"", NodeFileType::Directory, {}, {}, "", false, false, false};
    Node system{"system", NodeFileType::Directory, {}, {}, "", false, false, false};
    system.module_path = "/system";  // Set source for attribute cloning

    bool has_file = false;
    std::vector<std::string> failed_modules;

    LOG_INFO("Collecting files from modules directory");

    for (const auto& module_path : module_paths) {
        std::string module_id = module_path.filename().string();

        // Check if module is disabled or should be skipped
        if (fs::exists(module_path / "disable") || fs::exists(module_path / "remove") ||
            fs::exists(module_path / "skip_mount")) {
            LOG_DEBUG("Skipped module " + module_id + " (disabled/removed/skip_mount)");
            continue;
        }

        // Check if module has system partition
        fs::path module_system = module_path / "system";
        if (!fs::is_directory(module_system)) {
            LOG_DEBUG("Module " + module_id + " has no system directory");
            continue;
        }

        LOG_INFO("Processing module: " + module_id);
        try {
            bool module_has_file = collect_module_files(system, module_system, module_id);
            has_file |= module_has_file;
            if (module_has_file) {
                LOG_INFO("  Module " + module_id + " has files to mount");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to collect module " + module_id + ": " + std::string(e.what()));
            failed_modules.push_back(module_id);
            continue;
        }
    }

    if (!failed_modules.empty()) {
        LOG_WARN("Failed to process " + std::to_string(failed_modules.size()) + " module(s)");
    }

    if (!has_file) {
        LOG_WARN("No files to magic mount from any module");
        delete root;
        return nullptr;
    }

    LOG_INFO("File collection successful");

    const std::vector<std::pair<std::string, bool>> BUILTIN_PARTS = {
        {"vendor", true}, {"system_ext", true}, {"product", true}, {"odm", false}};

    for (const auto& [partition, require_symlink] : BUILTIN_PARTS) {
        fs::path path_of_root = fs::path("/") / partition;
        fs::path path_of_system = fs::path("/system") / partition;

        if (fs::is_directory(path_of_root) &&
            (!require_symlink || fs::is_symlink(path_of_system))) {
            auto it = system.children.find(partition);
            if (it != system.children.end()) {
                Node& node = it->second;
                if (node.file_type == NodeFileType::Symlink) {
                    if (fs::is_directory(node.module_path)) {
                        node.file_type = NodeFileType::Directory;
                    }
                }

                if (node.module_path.empty()) {
                    node.module_path = path_of_root;
                }

                root->children[partition] = node;
                system.children.erase(it);
            }
        }
    }

    for (const auto& partition : extra_partitions) {
        bool skip = false;
        for (const auto& [part, _] : BUILTIN_PARTS) {
            if (part == partition) {
                skip = true;
                break;
            }
        }
        if (skip || partition == "system") {
            continue;
        }

        fs::path path_of_root = fs::path("/") / partition;
        if (fs::is_directory(path_of_root)) {
            auto it = system.children.find(partition);
            if (it != system.children.end()) {
                LOG_DEBUG("attach extra partition '" + partition + "' to root");
                Node& node = it->second;
                if (node.file_type == NodeFileType::Symlink && fs::is_directory(node.module_path)) {
                    node.file_type = NodeFileType::Directory;
                }
                if (node.module_path.empty()) {
                    node.module_path = path_of_root;
                }
                root->children[partition] = node;
                system.children.erase(it);
            }
        }
    }

    root->children["system"] = system;
    return root;
}

static bool mount_mirror(const fs::path& src_path, const fs::path& dst_path,
                         const std::string& name) {
    fs::path src = src_path / name;
    fs::path dst = dst_path / name;

    try {
        struct stat st;
        if (lstat(src.c_str(), &st) != 0) {
            LOG_WARN("lstat failed for: " + src.string());
            return false;
        }

        if (S_ISREG(st.st_mode)) {
            // Regular file: create empty file then bind mount
            int fd = open(dst.c_str(), O_CREAT | O_WRONLY, st.st_mode & 07777);
            if (fd < 0) {
                LOG_ERROR("Failed to create mirror file: " + dst.string());
                return false;
            }
            close(fd);

            if (!mount_bind_modern(src, dst, true)) {
                LOG_WARN("Failed to bind mirror file: " + src.string());
                return false;
            }
            LOG_VERBOSE("Mirror file: " + src.string() + " -> " + dst.string());
        } else if (S_ISDIR(st.st_mode)) {
            // Directory: create dir, copy attributes, recursively mirror children
            if (mkdir(dst.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
                LOG_ERROR("Failed to create mirror directory: " + dst.string());
                return false;
            }

            chmod(dst.c_str(), st.st_mode & 07777);
            chown(dst.c_str(), st.st_uid, st.st_gid);
            clone_attr(src, dst);

            // Recursively mirror all children
            bool ok = true;
            for (const auto& entry : fs::directory_iterator(src)) {
                std::string child_name = entry.path().filename().string();
                if (!mount_mirror(src, dst, child_name)) {
                    ok = false;
                }
            }
            if (!ok) {
                return false;
            }
        } else if (S_ISLNK(st.st_mode)) {
            // Symlink: read target and create symlink
            char target[PATH_MAX];
            ssize_t len = readlink(src.c_str(), target, sizeof(target) - 1);
            if (len < 0) {
                LOG_ERROR("Failed to read symlink: " + src.string());
                return false;
            }
            target[len] = '\0';

            if (symlink(target, dst.c_str()) != 0) {
                LOG_ERROR("Failed to create symlink: " + dst.string());
                return false;
            }
            clone_attr(src, dst);
            LOG_VERBOSE("Mirror symlink: " + src.string() + " -> " + std::string(target));
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to mirror " + src.string() + ": " + std::string(e.what()));
        return false;
    }

    return true;
}

static bool mount_file(const fs::path& path, const fs::path& work_dir_path, const Node& node,
                       bool has_tmpfs, bool disable_umount) {
    g_mount_stats.total_mounts++;
    g_mount_stats.files_mounted++;

    fs::path target_path = has_tmpfs ? work_dir_path : path;

    if (has_tmpfs) {
        std::ofstream f(work_dir_path);
        f.close();
    }

    if (!node.module_path.empty()) {
        if (!mount_bind_modern(node.module_path, target_path, true)) {
            LOG_ERROR("Failed to bind mount file: " + node.module_path.string() + " -> " +
                      target_path.string());
            g_mount_stats.failed_mounts++;
            return false;
        }
        LOG_VERBOSE("Mount file: " + node.module_path.string() + " -> " + target_path.string());

        if (!disable_umount) {
            send_unmountable(target_path);
        }

        mount(nullptr, target_path.c_str(), nullptr, MS_REMOUNT | MS_RDONLY | MS_BIND, nullptr);
        g_mount_stats.successful_mounts++;
    }

    return true;
}

static bool mount_symlink(const fs::path& work_dir_path, const Node& node) {
    g_mount_stats.total_mounts++;
    g_mount_stats.symlinks_created++;

    if (!node.module_path.empty()) {
        try {
            auto link_target = fs::read_symlink(node.module_path);

            // Validate symlink safety
            if (!is_safe_symlink(node.module_path, fs::path("/"))) {
                LOG_ERROR("Unsafe symlink detected: " + node.module_path.string());
                g_mount_stats.failed_mounts++;
                return false;
            }

            fs::create_symlink(link_target, work_dir_path);
            clone_attr(node.module_path, work_dir_path);
            g_mount_stats.successful_mounts++;
        } catch (...) {
            g_mount_stats.failed_mounts++;
            return false;
        }
    }
    return true;
}

static bool create_whiteout(const fs::path& target_path, const fs::path& work_dir_path) {
    try {
        fs::create_directories(work_dir_path.parent_path());

        if (fs::exists(work_dir_path)) {
            fs::remove(work_dir_path);
        }

        if (mknod(work_dir_path.c_str(), S_IFCHR | 0000, makedev(0, 0)) != 0) {
            LOG_ERROR("Failed to create whiteout: " + work_dir_path.string() + ": " +
                      strerror(errno));
            return false;
        }

        if (fs::exists(target_path)) {
            clone_attr(target_path, work_dir_path);
        } else {
            copy_path_context(work_dir_path.parent_path(), work_dir_path);
        }

        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create whiteout: " + work_dir_path.string() + ": " +
                  std::string(e.what()));
        return false;
    }
}

static bool do_magic_mount(const fs::path& path, const fs::path& work_dir_path, const Node& current,
                           bool has_tmpfs, bool disable_umount);

static bool mount_directory_children(const fs::path& path, const fs::path& work_dir_path,
                                     const Node& node, bool has_tmpfs, bool disable_umount) {
    bool ok = true;
    if (fs::exists(path) && !node.replace) {
        try {
            for (const auto& entry : fs::directory_iterator(path)) {
                std::string name = entry.path().filename().string();
                auto it = node.children.find(name);
                if (it != node.children.end()) {
                    if (!it->second.skip) {
                        if (!do_magic_mount(path, work_dir_path, it->second, has_tmpfs,
                                            disable_umount)) {
                            ok = false;
                        }
                    }
                } else if (has_tmpfs) {
                    if (!mount_mirror(path, work_dir_path, name)) {
                        ok = false;
                    }
                }
            }
        } catch (...) {
            LOG_WARN("Failed to iterate directory: " + path.string());
            ok = false;
        }
    }

    for (const auto& [name, child_node] : node.children) {
        if (child_node.skip) {
            continue;
        }

        fs::path real_path = path / name;
        if (!fs::exists(real_path) && !node.replace) {
            if (!do_magic_mount(path, work_dir_path, child_node, has_tmpfs, disable_umount)) {
                ok = false;
            }
        }
    }

    return ok;
}

static bool should_create_tmpfs(const Node& node, const fs::path& path, bool has_tmpfs) {
    if (has_tmpfs) {
        return true;
    }

    if (node.replace) {
        return fs::exists(path) || !node.module_path.empty();
    }

    for (const auto& [name, child] : node.children) {
        fs::path real_path = path / name;

        bool need = false;
        if (child.file_type == NodeFileType::Symlink) {
            need = true;
        } else if (child.file_type == NodeFileType::Whiteout) {
            need = fs::exists(real_path);
        } else {
            try {
                if (fs::exists(real_path)) {
                    NodeFileType real_ft = get_file_type(real_path);
                    need = (real_ft != child.file_type || real_ft == NodeFileType::Symlink);
                } else {
                    need = true;
                }
            } catch (...) {
                need = true;
            }
        }

        if (need) {
            if (node.module_path.empty() && !fs::exists(path)) {
                LOG_ERROR("Cannot create tmpfs on " + path.string() + " (no source)");
                return false;
            }
            return true;
        }
    }

    return false;
}

static bool prepare_tmpfs_dir(const fs::path& path, const fs::path& work_dir_path,
                              const Node& node) {
    try {
        fs::create_directories(work_dir_path);

        if (!fs::exists(path) && node.module_path.empty()) {
            LOG_ERROR("No source for tmpfs skeleton: " + path.string());
            return false;
        }

        fs::path src_path = fs::exists(path) ? path : node.module_path;
        clone_attr(src_path, work_dir_path);

        mount(work_dir_path.c_str(), work_dir_path.c_str(), nullptr, MS_BIND | MS_REC, nullptr);
    } catch (...) {
        return false;
    }

    return true;
}

static bool finalize_tmpfs_overlay(const fs::path& path, const fs::path& work_dir_path,
                                   bool disable_umount) {
    mount(nullptr, work_dir_path.c_str(), nullptr, MS_REMOUNT | MS_RDONLY | MS_BIND, nullptr);
    mount(work_dir_path.c_str(), path.c_str(), nullptr, MS_MOVE, nullptr);
    mount(nullptr, path.c_str(), nullptr, MS_PRIVATE, nullptr);

    if (!disable_umount) {
        send_unmountable(path);
    }

    LOG_VERBOSE("Finalized tmpfs overlay: " + work_dir_path.string() + " -> " + path.string());
    return true;
}

static bool do_magic_mount(const fs::path& path, const fs::path& work_dir_path, const Node& current,
                           bool has_tmpfs, bool disable_umount) {
    fs::path target_path = path / current.name;
    fs::path target_work_path = work_dir_path / current.name;

    switch (current.file_type) {
    case NodeFileType::RegularFile:
        return mount_file(target_path, target_work_path, current, has_tmpfs, disable_umount);

    case NodeFileType::Symlink:
        if (has_tmpfs) {
            return mount_symlink(target_work_path, current);
        } else {
            return mount_file(target_path, target_work_path, current, has_tmpfs, disable_umount);
        }

    case NodeFileType::Directory: {
        g_mount_stats.dirs_mounted++;
        bool create_tmpfs = !has_tmpfs && should_create_tmpfs(current, target_path, false);
        bool effective_tmpfs = has_tmpfs || create_tmpfs;

        if (effective_tmpfs) {
            if (create_tmpfs) {
                if (!prepare_tmpfs_dir(target_path, target_work_path, current)) {
                    g_mount_stats.failed_mounts++;
                    return false;
                }
            } else if (has_tmpfs && !fs::exists(target_work_path)) {
                fs::create_directory(target_work_path);
                fs::path src_path = fs::exists(target_path) ? target_path : current.module_path;
                clone_attr(src_path, target_work_path);
            }
        }

        if (!mount_directory_children(target_path, target_work_path, current, effective_tmpfs,
                                      disable_umount)) {
            g_mount_stats.failed_mounts++;
            return false;
        }

        if (create_tmpfs) {
            if (!finalize_tmpfs_overlay(target_path, target_work_path, disable_umount)) {
                g_mount_stats.failed_mounts++;
                return false;
            }
        }
        break;
    }

    case NodeFileType::Whiteout:
        if (has_tmpfs) {
            if (!create_whiteout(target_path, target_work_path)) {
                g_mount_stats.failed_mounts++;
                return false;
            }
            g_mount_stats.successful_mounts++;
        }
        break;
    }

    return true;
}

bool mount_partitions(const fs::path& tmp_path, const std::vector<fs::path>& module_paths,
                      const std::string& mount_source,
                      const std::vector<std::string>& extra_partitions, bool disable_umount) {
    Node* root = collect_all_modules(module_paths, extra_partitions);
    if (!root) {
        LOG_INFO("No files to magic mount");
        return true;
    }

    fs::path work_dir = tmp_path / "workdir";

    if (!mount_tmpfs(work_dir)) {
        LOG_ERROR("Failed to create workdir tmpfs at " + work_dir.string());
        delete root;
        return false;
    }

    mount(nullptr, work_dir.c_str(), nullptr, MS_PRIVATE, nullptr);

    bool result = false;
    try {
        result = do_magic_mount("/", work_dir, *root, false, disable_umount);
    } catch (const std::exception& e) {
        LOG_ERROR("Magic mount failed with exception: " + std::string(e.what()));
        result = false;
    } catch (...) {
        LOG_ERROR("Magic mount failed with unknown exception");
        result = false;
    }

    g_mount_stats.tmpfs_created++;
    if (umount2(work_dir.c_str(), MNT_DETACH) != 0) {
        LOG_WARN("Failed to umount workdir: " + work_dir.string() + ": " + strerror(errno));
    }
    try {
        if (fs::exists(work_dir)) {
            fs::remove(work_dir);
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to remove workdir: " + work_dir.string() + ": " + e.what());
    }

    delete root;

    save_mount_statistics();

    return result;
}

bool mount_partitions_auto(const fs::path& tmp_path, const std::vector<fs::path>& module_paths,
                           const std::string& mount_source, bool disable_umount) {
    // Automatically detect all partitions
    LOG_INFO("Detecting partitions from /proc/mounts");
    auto all_partitions = detect_partitions();
    auto extra_partitions = get_extra_partitions(all_partitions);

    LOG_INFO("Detected " + std::to_string(all_partitions.size()) + " partitions, " +
             std::to_string(extra_partitions.size()) + " extra partitions");

    return mount_partitions(tmp_path, module_paths, mount_source, extra_partitions, disable_umount);
}

MountStatistics get_mount_statistics() {
    // Load from JSON file
    MountStatistics stats;
    std::ifstream file(MOUNT_STATS_FILE);
    if (file.is_open()) {
        try {
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            file.close();

            // Simple JSON parsing
            auto get_int = [&content](const std::string& key) -> int {
                auto pos = content.find("\"" + key + "\":");
                if (pos == std::string::npos)
                    return 0;
                pos = content.find(":", pos) + 1;
                auto end = content.find_first_of(",}", pos);
                return std::stoi(content.substr(pos, end - pos));
            };

            stats.total_mounts = get_int("total_mounts");
            stats.successful_mounts = get_int("successful_mounts");
            stats.failed_mounts = get_int("failed_mounts");
            stats.tmpfs_created = get_int("tmpfs_created");
            stats.files_mounted = get_int("files_mounted");
            stats.dirs_mounted = get_int("dirs_mounted");
            stats.symlinks_created = get_int("symlinks_created");
            stats.overlayfs_mounts = get_int("overlayfs_mounts");
        } catch (...) {
            // Return zeros on parse error
        }
    }

    return stats;
}

void save_mount_statistics() {
    std::ofstream file(MOUNT_STATS_FILE);
    if (!file.is_open()) {
        LOG_WARN("Failed to save mount statistics");
        return;
    }

    file << "{\n"
         << "  \"total_mounts\": " << g_mount_stats.total_mounts << ",\n"
         << "  \"successful_mounts\": " << g_mount_stats.successful_mounts << ",\n"
         << "  \"failed_mounts\": " << g_mount_stats.failed_mounts << ",\n"
         << "  \"tmpfs_created\": " << g_mount_stats.tmpfs_created << ",\n"
         << "  \"files_mounted\": " << g_mount_stats.files_mounted << ",\n"
         << "  \"dirs_mounted\": " << g_mount_stats.dirs_mounted << ",\n"
         << "  \"symlinks_created\": " << g_mount_stats.symlinks_created << ",\n"
         << "  \"overlayfs_mounts\": " << g_mount_stats.overlayfs_mounts << "\n"
         << "}\n";

    file.close();
}

void increment_overlay_stats() {
    g_mount_stats.overlayfs_mounts++;
}

void reset_mount_statistics() {
    g_mount_stats = MountStats();
    save_mount_statistics();
}

}  // namespace hymo
