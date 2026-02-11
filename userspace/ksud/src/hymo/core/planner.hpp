// core/planner.hpp - Mount planning
#pragma once

#include "../conf/config.hpp"
#include "inventory.hpp"
#include <filesystem>
#include <map>
#include <vector>

namespace fs = std::filesystem;

namespace hymo {

struct OverlayOperation {
  std::string target;
  std::vector<fs::path>
      lowerdirs; // Ordered from top to bottom (higher priority first)
};

struct MountPlan {
  std::vector<OverlayOperation> overlay_ops;
  std::vector<fs::path> magic_module_paths;
  std::vector<std::string> overlay_module_ids;
  std::vector<std::string> magic_module_ids;
  std::vector<std::string> hymofs_module_ids;

  bool is_covered_by_overlay(const std::string &path) const;
};

MountPlan generate_plan(const Config &config,
                        const std::vector<Module> &modules,
                        const fs::path &storage_root);

void update_hymofs_mappings(const Config &config,
                            const std::vector<Module> &modules,
                            const fs::path &storage_root, MountPlan &plan);

} // namespace hymo
