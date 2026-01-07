// core/storage.hpp - Storage backend management (FIXED)
#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace hymo {

struct StorageHandle {
    fs::path mount_point;
    std::string mode;  // "tmpfs", "ext4", or "erofs"
};

StorageHandle setup_storage(const fs::path& mnt_dir, const fs::path& image_path, bool force_ext4,
                            bool prefer_erofs = false);

// New: Finalize storage permission repair (called after sync)
void finalize_storage_permissions(const fs::path& storage_root);

void print_storage_status();

}  // namespace hymo