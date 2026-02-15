// mount/mount_utils.cpp - Mount utility functions implementation
#include "mount_utils.hpp"
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>
#include "../defs.hpp"
#include "../utils.hpp"

namespace {
#ifndef AT_RECURSIVE
constexpr unsigned int MOUNT_AT_RECURSIVE = 0x8000;
#else
constexpr unsigned int MOUNT_AT_RECURSIVE = AT_RECURSIVE;
#endif  // #ifndef AT_RECURSIVE
#ifndef OPEN_TREE_CLONE
constexpr int OPEN_TREE_CLONE_VAL = 1;
#else
constexpr int OPEN_TREE_CLONE_VAL = OPEN_TREE_CLONE;
#endif  // #ifndef OPEN_TREE_CLONE
#ifndef MOVE_MOUNT_F_EMPTY_PATH
constexpr unsigned int MOVE_MOUNT_F_EMPTY_PATH_VAL = 0x00000004;
#else
constexpr unsigned int MOVE_MOUNT_F_EMPTY_PATH_VAL = MOVE_MOUNT_F_EMPTY_PATH;
#endif  // #ifndef MOVE_MOUNT_F_EMPTY_PATH
}  // namespace

namespace hymo {

bool clone_attr(const fs::path& source, const fs::path& target) {
    struct stat st{};
    if (lstat(source.c_str(), &st) != 0) {
        LOG_ERROR("Failed to stat source: " + source.string() + " - " + strerror(errno));
        return false;
    }

    // Set owner and group
    if (lchown(target.c_str(), st.st_uid, st.st_gid) != 0) {
        LOG_WARN("Failed to chown " + target.string() + ": " + strerror(errno));
    }

    // Set permissions (for non-symlinks)
    if (!S_ISLNK(st.st_mode)) {
        if (chmod(target.c_str(), st.st_mode & 07777) != 0) {
            LOG_WARN("Failed to chmod " + target.string() + ": " + strerror(errno));
        }
    }

    // Set timestamps
    std::array<struct timespec, 2> times{};
    times[0] = st.st_atim;  // access time
    times[1] = st.st_mtim;  // modification time

    if (utimensat(AT_FDCWD, target.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) != 0) {
        LOG_WARN("Failed to set times on " + target.string() + ": " + strerror(errno));
    }

#ifdef __ANDROID__
    // Copy SELinux context only if source has one
    ssize_t sel_len = lgetxattr(source.c_str(), SELINUX_XATTR, nullptr, 0);
    if (sel_len > 0) {
        std::string context;
        context.resize(static_cast<size_t>(sel_len));
        if (lgetxattr(source.c_str(), SELINUX_XATTR, context.data(), sel_len) > 0) {
            if (!lsetfilecon(target, context)) {
                LOG_WARN("Failed to set SELinux context on " + target.string() + ": " +
                         strerror(errno));
            }
        } else {
            LOG_WARN("Failed to read SELinux context from " + source.string() + ": " +
                     strerror(errno));
        }
    }
#endif  // #ifdef __ANDROID__

    // Copy extended attributes (except security.selinux which we already copied)
    const ssize_t list_size = llistxattr(source.c_str(), nullptr, 0);

    if (list_size > 0) {
        std::vector<char> list(static_cast<size_t>(list_size));
        if (llistxattr(source.c_str(), list.data(), list_size) > 0) {
            for (char* name = list.data(); name < list.data() + list_size;
                 name += strlen(name) + 1) {
                // Skip security.selinux as we already copied it with lgetfilecon
                if (strcmp(name, "security.selinux") == 0) {
                    continue;
                }

                // Get xattr value
                const ssize_t val_size = lgetxattr(source.c_str(), name, nullptr, 0);
                if (val_size > 0) {
                    std::vector<char> value(static_cast<size_t>(val_size));
                    if (lgetxattr(source.c_str(), name, value.data(), val_size) > 0) {
                        if (lsetxattr(target.c_str(), name, value.data(),
                                      static_cast<size_t>(val_size), 0) != 0) {
                            LOG_WARN("Failed to set xattr " + std::string(name) + " on " +
                                     target.string() + ": " + strerror(errno));
                        }
                    }
                }
            }
        }
    }

    return true;
}

namespace {

// Modern mount using open_tree + move_mount
bool try_modern_bind_mount(const fs::path& source, const fs::path& target, bool recursive) {
#ifdef __NR_open_tree
    int flags = OPEN_TREE_CLONE_VAL | AT_EMPTY_PATH;
    if (recursive) {
        flags |= static_cast<int>(MOUNT_AT_RECURSIVE);
    }

    const int tree_fd = syscall(__NR_open_tree, AT_FDCWD, source.c_str(), flags);
    if (tree_fd < 0) {
        return false;
    }

    const int ret = syscall(__NR_move_mount, tree_fd, "", AT_FDCWD, target.c_str(),
                            MOVE_MOUNT_F_EMPTY_PATH_VAL);
    close(tree_fd);

    return ret == 0;
#else
    return false;
#endif  // #ifdef __NR_open_tree
}

}  // namespace

bool mount_bind_modern(const fs::path& source, const fs::path& target, bool recursive) {
    // Try modern API first (kernel 5.2+)
    if (try_modern_bind_mount(source, target, recursive)) {
        return true;
    }

    // Fallback to traditional mount
    unsigned long flags = MS_BIND;
    if (recursive) {
        flags |= MS_REC;
    }

    if (mount(source.c_str(), target.c_str(), nullptr, flags, nullptr) == 0) {
        return true;
    }

    LOG_ERROR("Bind mount failed: " + source.string() + " -> " + target.string() + " - " +
              strerror(errno));
    return false;
}

bool mount_with_retry(const char* source, const char* target, const char* filesystemtype,
                      unsigned long mountflags, const void* data, int max_retries) {
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        if (mount(source, target, filesystemtype, mountflags, data) == 0) {
            if (attempt > 0) {
                LOG_INFO("Mount succeeded on attempt " + std::to_string(attempt + 1));
            }
            return true;
        }

        const int err = errno;
        if (attempt < max_retries - 1) {
            LOG_WARN("Mount attempt " + std::to_string(attempt + 1) + " failed: " + strerror(err) +
                     ", retrying...");
            std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
        } else {
            LOG_ERROR("Mount failed after " + std::to_string(max_retries) +
                      " attempts: " + strerror(err));
        }
    }

    return false;
}

bool is_safe_path(const fs::path& base, const fs::path& target) {
    try {
        auto canonical_base = fs::canonical(base);
        auto canonical_target = fs::canonical(target);

        // Check if target is within base directory
        auto target_str = canonical_target.string();
        auto base_str = canonical_base.string();

        // C++17 compatible starts_with
        return target_str.compare(0, base_str.length(), base_str) == 0;
    } catch (const fs::filesystem_error& e) {
        LOG_WARN("Path validation failed: " + std::string(e.what()));
        return false;
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) link_path vs base are distinct
bool is_safe_symlink(const fs::path& link_path, const fs::path& base) {
    (void)base;
    try {
        if (!fs::is_symlink(link_path)) {
            return true;
        }

        auto target = fs::read_symlink(link_path);

        // Check for absolute paths pointing to sensitive directories
        if (target.is_absolute()) {
            const std::string target_str = target.string();
            const std::vector<std::string> forbidden_prefixes = {"/data/", "/dev/", "/proc/",
                                                                 "/sys/"};

            for (const auto& prefix : forbidden_prefixes) {
                // C++17 compatible starts_with
                if (target_str.compare(0, prefix.length(), prefix) == 0) {
                    LOG_WARN("Suspicious symlink target: " + target_str);
                    return false;
                }
            }
        }

        // Check symlink depth to prevent cycles
        int depth = 0;
        fs::path current = link_path;
        while (fs::is_symlink(current) && depth < 20) {
            current = fs::read_symlink(current);
            if (!current.is_absolute()) {
                current = link_path.parent_path() / current;
            }
            depth++;
        }

        if (depth >= 20) {
            LOG_WARN("Symlink depth exceeded: " + link_path.string());
            return false;
        }

        return true;
    } catch (const fs::filesystem_error& e) {
        LOG_WARN("Symlink validation failed: " + std::string(e.what()));
        return false;
    }
}

FastFileType get_file_type_fast(const fs::directory_entry& entry) {
    // Try to use cached file type from readdir first
    try {
        auto status = entry.symlink_status();
        auto type = status.type();

        if (type == fs::file_type::directory) {
            return FastFileType::Directory;
        } else if (type == fs::file_type::symlink) {
            return FastFileType::Symlink;
        } else if (type == fs::file_type::regular) {
            return FastFileType::RegularFile;
        } else if (type == fs::file_type::character) {
            return FastFileType::CharDevice;
        } else if (type == fs::file_type::block) {
            return FastFileType::BlockDevice;
        } else if (type == fs::file_type::fifo) {
            return FastFileType::Fifo;
        } else if (type == fs::file_type::socket) {
            return FastFileType::Socket;
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch) ignore symlink_status failure
    }

    return FastFileType::Unknown;
}

}  // namespace hymo
