// mount/magic.hpp - Magic mount implementation
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hymo {

// Mount partitions using magic mount (recursive bind mount with tmpfs)
bool mount_partitions(const fs::path& tmp_path, const std::vector<fs::path>& module_paths,
                      const std::string& mount_source,
                      const std::vector<std::string>& extra_partitions, bool disable_umount);

}  // namespace hymo
