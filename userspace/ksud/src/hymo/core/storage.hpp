// core/storage.hpp - Storage management
#pragma once

#include <filesystem>
#include <string>
#include "../conf/config.hpp"

namespace fs = std::filesystem;

namespace hymo {

struct StorageHandle {
    fs::path mount_point;
    std::string mode;  // tmpfs, ext4, erofs
};

StorageHandle setup_storage(const fs::path& mnt_dir, const fs::path& image_path,
                            FilesystemType fs_type);

// Exposed for CLI tools
bool create_image(const fs::path& base_dir);

void finalize_storage_permissions(const fs::path& storage_root);

void print_storage_status();

}  // namespace hymo