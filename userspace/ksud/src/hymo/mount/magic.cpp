// mount/magic.cpp - Magic mount implementation
#include "magic.hpp"
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include "../hymo_defs.hpp"
#include "../hymo_utils.hpp"

namespace hymo {

enum class NodeFileType { RegularFile, Directory, Symlink, Whiteout };

struct Node {
    std::string name;
    NodeFileType file_type;
    std::unordered_map<std::string, Node> children;
    fs::path module_path;
    bool replace = false;
    bool skip = false;
};

static bool dir_is_replace(const fs::path& path) {
    // Check for xattr
    char buf[4];
    ssize_t len = lgetxattr(path.c_str(), REPLACE_DIR_XATTR, buf, sizeof(buf));
    if (len > 0 && buf[0] == 'y') {
        return true;
    }

    // Check for .replace file
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

static bool collect_module_files(Node& node, const fs::path& module_dir) {
    if (!fs::exists(module_dir) || !fs::is_directory(module_dir)) {
        return false;
    }

    bool has_file = false;

    try {
        for (const auto& entry : fs::directory_iterator(module_dir)) {
            std::string name = entry.path().filename().string();
            NodeFileType ft = get_file_type(entry.path());

            Node child;
            child.name = name;
            child.file_type = ft;
            child.module_path = entry.path();

            if (ft == NodeFileType::Directory) {
                child.replace = dir_is_replace(entry.path());
                has_file |= collect_module_files(child, entry.path()) || child.replace;
            } else {
                has_file = true;
            }

            node.children[name] = child;
        }
    } catch (...) {
        return false;
    }

    return has_file;
}

static Node* collect_all_modules(const std::vector<fs::path>& content_paths,
                                 const std::vector<std::string>& extra_partitions) {
    Node* root = new Node{"", NodeFileType::Directory, {}, {}, false, false};
    Node system{"system", NodeFileType::Directory, {}, {}, false, false};
    system.module_path = "/system";  // Set source for attribute cloning

    bool has_file = false;

    for (const auto& module_path : content_paths) {
        fs::path module_system = module_path / "system";
        if (!fs::is_directory(module_system)) {
            continue;
        }

        LOG_DEBUG("collecting " + module_path.string());
        has_file |= collect_module_files(system, module_system);
    }

    if (!has_file) {
        delete root;
        return nullptr;
    }

    // Move standard partitions to root
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
                // Ensure moved partition nodes have a valid module_path (source)
                // If it was a symlink/directory from a module, it has one.
                // But if it's a synthetic node, we might need to point to real
                // partition.
                if (node.module_path.empty()) {
                    node.module_path = path_of_root;
                }

                root->children[partition] = node;
                system.children.erase(it);
            }
        }
    }

    // Handle extra partitions
    for (const auto& partition : extra_partitions) {
        // Skip if already processed
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

static bool mount_mirror(const fs::path& path, const fs::path& work_dir_path,
                         const fs::directory_entry& entry) {
    fs::path target_path = path / entry.path().filename();
    fs::path work_path = work_dir_path / entry.path().filename();

    try {
        if (entry.is_regular_file()) {
            std::ofstream f(work_path);
            f.close();
            mount(target_path.c_str(), work_path.c_str(), nullptr, MS_BIND, nullptr);
        } else if (entry.is_directory()) {
            fs::create_directory(work_path);
            auto perms = fs::status(entry.path()).permissions();
            fs::permissions(work_path, perms);
            copy_path_context(target_path, work_path);

            for (const auto& sub_entry : fs::directory_iterator(target_path)) {
                mount_mirror(target_path, work_path, sub_entry);
            }
        } else if (entry.is_symlink()) {
            auto link_target = fs::read_symlink(entry.path());
            fs::create_symlink(link_target, work_path);
            copy_path_context(target_path, work_path);
        }
    } catch (...) {
        return false;
    }

    return true;
}

static bool mount_file(const fs::path& path, const fs::path& work_dir_path, const Node& node,
                       bool has_tmpfs, bool disable_umount) {
    fs::path target_path = has_tmpfs ? work_dir_path : path;

    if (has_tmpfs) {
        std::ofstream f(work_dir_path);
        f.close();
    }

    if (!node.module_path.empty()) {
        mount(node.module_path.c_str(), target_path.c_str(), nullptr, MS_BIND, nullptr);
        if (!disable_umount) {
            send_unmountable(target_path);
        }
        mount(nullptr, target_path.c_str(), nullptr, MS_REMOUNT | MS_RDONLY | MS_BIND, nullptr);
    }

    return true;
}

static bool mount_symlink(const fs::path& work_dir_path, const Node& node) {
    if (!node.module_path.empty()) {
        try {
            auto link_target = fs::read_symlink(node.module_path);
            fs::create_symlink(link_target, work_dir_path);
            copy_path_context(node.module_path, work_dir_path);
        } catch (...) {
            return false;
        }
    }
    return true;
}

static bool do_magic_mount(const fs::path& path, const fs::path& work_dir_path, const Node& current,
                           bool has_tmpfs, bool disable_umount);

static bool mount_directory_children(const fs::path& path, const fs::path& work_dir_path,
                                     const Node& node, bool has_tmpfs, bool disable_umount) {
    // Mirror existing files if using tmpfs and not replacing
    if (has_tmpfs && fs::exists(path) && !node.replace) {
        try {
            for (const auto& entry : fs::directory_iterator(path)) {
                std::string name = entry.path().filename().string();
                if (node.children.find(name) == node.children.end()) {
                    mount_mirror(path, work_dir_path, entry);
                }
            }
        } catch (...) {
        }
    }

    // Mount module children
    for (const auto& [name, child_node] : node.children) {
        if (child_node.skip) {
            continue;
        }
        do_magic_mount(path, work_dir_path, child_node, has_tmpfs, disable_umount);
    }

    return true;
}

static bool should_create_tmpfs(const Node& node, const fs::path& path, bool has_tmpfs) {
    if (has_tmpfs) {
        return true;
    }

    if (node.replace && !node.module_path.empty()) {
        return true;
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
            if (node.module_path.empty()) {
                LOG_ERROR("Cannot create tmpfs on " + path.string() + " (no module source)");
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

        fs::path src_path = fs::exists(path) ? path : node.module_path;
        auto perms = fs::status(src_path).permissions();

        fs::permissions(work_dir_path, perms);
        copy_path_context(src_path, work_dir_path);

        mount(work_dir_path.c_str(), work_dir_path.c_str(), nullptr, MS_BIND, nullptr);
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
        return mount_symlink(target_work_path, current);

    case NodeFileType::Directory: {
        bool create_tmpfs = !has_tmpfs && should_create_tmpfs(current, target_path, false);
        bool effective_tmpfs = has_tmpfs || create_tmpfs;

        if (effective_tmpfs) {
            if (create_tmpfs) {
                prepare_tmpfs_dir(target_path, target_work_path, current);
            } else if (has_tmpfs && !fs::exists(target_work_path)) {
                fs::create_directory(target_work_path);
                fs::path src_path = fs::exists(target_path) ? target_path : current.module_path;
                auto perms = fs::status(src_path).permissions();
                fs::permissions(target_work_path, perms);
                copy_path_context(src_path, target_work_path);
            }
        }

        mount_directory_children(target_path, target_work_path, current, effective_tmpfs,
                                 disable_umount);

        if (create_tmpfs) {
            finalize_tmpfs_overlay(target_path, target_work_path, disable_umount);
        }
        break;
    }

    case NodeFileType::Whiteout:
        // Skip whiteout files
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

    fs::path tmp_dir = tmp_path / "workdir";
    ensure_dir_exists(tmp_dir);

    mount(mount_source.c_str(), tmp_dir.c_str(), "tmpfs", 0, "");
    mount(nullptr, tmp_dir.c_str(), nullptr, MS_PRIVATE, nullptr);

    bool result = do_magic_mount("/", tmp_dir, *root, false, disable_umount);

    umount2(tmp_dir.c_str(), MNT_DETACH);
    fs::remove(tmp_dir);

    delete root;

    return result;
}

}  // namespace hymo
