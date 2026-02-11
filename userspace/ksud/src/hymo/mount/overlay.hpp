// mount/overlay.hpp - OverlayFS mounting
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hymo {

// Mount overlayfs on target with given lowerdirs
bool mount_overlay(const std::string &target_root,
                   const std::vector<std::string> &module_roots,
                   const std::string &mount_source,
                   std::optional<fs::path> upperdir,
                   std::optional<fs::path> workdir, bool disable_umount,
                   const std::vector<std::string> &partitions = {});

// Bind mount helper
bool bind_mount(const fs::path &from, const fs::path &to, bool disable_umount);

} // namespace hymo
