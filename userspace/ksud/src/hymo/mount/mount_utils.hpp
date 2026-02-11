// mount/mount_utils.hpp - Mount utility functions
#pragma once

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <filesystem>
#include <string>
#include "../defs.hpp"

namespace fs = std::filesystem;

namespace hymo {

// Clone all attributes from source to target
// Includes: owner, permissions, timestamps, SELinux context, xattrs
bool clone_attr(const fs::path& source, const fs::path& target);

// Modern mount using open_tree + move_mount (kernel 5.2+)
// Falls back to traditional mount on failure
// Note: This function does NOT log - caller should log appropriately
bool mount_bind_modern(const fs::path& source, const fs::path& target, bool recursive = true);

// Mount with automatic retry and fallback
bool mount_with_retry(const char* source, const char* target, const char* filesystemtype,
                      unsigned long mountflags, const void* data, int max_retries = 3);

// Check if path is safe (within allowed base directory)
bool is_safe_path(const fs::path& base, const fs::path& target);

// Validate symlink target is safe
bool is_safe_symlink(const fs::path& link_path, const fs::path& base);

// Get file type using readdir DT_* constants (faster than stat)
enum class FastFileType {
    Unknown,
    RegularFile,
    Directory,
    Symlink,
    CharDevice,
    BlockDevice,
    Fifo,
    Socket
};

FastFileType get_file_type_fast(const fs::directory_entry& entry);

}  // namespace hymo
