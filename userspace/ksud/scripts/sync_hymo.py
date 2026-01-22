#!/usr/bin/env python3
"""
Sync hymo source code from standalone repository to ksud embedded version.
This script pulls the latest hymo code and adapts it for embedded use in ksud.
"""

import os
import sys
import shutil
import subprocess
from pathlib import Path

# Paths
SCRIPT_DIR = Path(__file__).parent
KSUD_DIR = SCRIPT_DIR.parent
YUKISU_DIR = KSUD_DIR.parent.parent  # YukiSU root
WORKSPACE_DIR = YUKISU_DIR.parent    # hymoworker
HYMO_REPO_URL = "https://github.com/Anatdx/meta-hymo"

# Try CI environment path first (YukiSU/meta-hymo), then local path (hymoworker/meta-hymo)
if (YUKISU_DIR / "meta-hymo").exists():
    HYMO_REPO_DIR = YUKISU_DIR / "meta-hymo"
else:
    HYMO_REPO_DIR = WORKSPACE_DIR / "meta-hymo"

HYMO_EMBEDDED_DIR = KSUD_DIR / "src" / "hymo"

# Files to sync (relative to hymo/src/)
HYMO_SOURCE_MAPPINGS = {
    # Configuration
    "conf/config.cpp": "conf/config.cpp",
    "conf/config.hpp": "conf/config.hpp",
    
    # Core functionality
    "core/executor.cpp": "core/executor.cpp",
    "core/executor.hpp": "core/executor.hpp",
    "core/inventory.cpp": "core/inventory.cpp",
    "core/inventory.hpp": "core/inventory.hpp",
    "core/json.hpp": "core/json.hpp",
    "core/modules.cpp": "core/modules.cpp",
    "core/modules.hpp": "core/modules.hpp",
    "core/planner.cpp": "core/planner.cpp",
    "core/planner.hpp": "core/planner.hpp",
    "core/state.cpp": "core/state.cpp",
    "core/state.hpp": "core/state.hpp",
    "core/storage.cpp": "core/storage.cpp",
    "core/storage.hpp": "core/storage.hpp",
    "core/sync.cpp": "core/sync.cpp",
    "core/sync.hpp": "core/sync.hpp",
    "core/user_rules.cpp": "core/user_rules.cpp",
    "core/user_rules.hpp": "core/user_rules.hpp",
    
    # Mount functionality
    "mount/hymo_magic.h": "mount/hymo_magic.h",
    "mount/hymofs.cpp": "mount/hymofs.cpp",
    "mount/hymofs.hpp": "mount/hymofs.hpp",
    "mount/magic.cpp": "mount/magic.cpp",
    "mount/magic.hpp": "mount/magic.hpp",
    "mount/overlay.cpp": "mount/overlay.cpp",
    "mount/overlay.hpp": "mount/overlay.hpp",
    
    # Utilities (need adaptation)
    "utils.cpp": "hymo_utils.cpp",
    "utils.hpp": "hymo_utils.hpp",
    "defs.hpp": "hymo_defs.hpp",
    
    # CLI (auto-converted, minimal changes)
    "main.cpp": "hymo_cli.cpp",
}

# Code transformations for embedded version
INCLUDE_REPLACEMENTS = {
    '#include "defs.hpp"': '#include "hymo_defs.hpp"',
    '#include "utils.hpp"': '#include "hymo_utils.hpp"',
    '#include "../defs.hpp"': '#include "../hymo_defs.hpp"',
    '#include "../utils.hpp"': '#include "../hymo_utils.hpp"',
    '#include "../../defs.hpp"': '#include "../../hymo_defs.hpp"',
    '#include "../../utils.hpp"': '#include "../../hymo_utils.hpp"',
}

def run_command(cmd, cwd=None):
    """Run shell command and return output."""
    result = subprocess.run(
        cmd,
        cwd=cwd,
        shell=True,
        capture_output=True,
        text=True
    )
    if result.returncode != 0:
        print(f"Error running command: {cmd}", file=sys.stderr)
        print(f"stderr: {result.stderr}", file=sys.stderr)
        return None
    return result.stdout.strip()

def pull_hymo_repo():
    """Pull latest hymo code from repository."""
    if not HYMO_REPO_DIR.exists():
        print(f"Error: hymo repository not found at {HYMO_REPO_DIR}", file=sys.stderr)
        print("Please clone hymo repository first:", file=sys.stderr)
        print(f"  git clone {HYMO_REPO_URL} {HYMO_REPO_DIR}", file=sys.stderr)
        return False
    
    print(f"Pulling latest hymo code from {HYMO_REPO_DIR}...")
    
    # Check if repo has uncommitted changes
    status = run_command("git status --porcelain", cwd=HYMO_REPO_DIR)
    if status:
        print("Warning: hymo repository has uncommitted changes", file=sys.stderr)
        response = input("Continue anyway? [y/N]: ")
        if response.lower() != 'y':
            return False
    
    # Pull latest changes
    output = run_command("git pull", cwd=HYMO_REPO_DIR)
    if output is None:
        print("Warning: Failed to pull latest changes, using current version")
    else:
        print(f"Git pull output: {output}")
    
    return True

def transform_code(content, filepath):
    """Apply transformations for embedded version."""
    # Replace include paths
    for old_include, new_include in INCLUDE_REPLACEMENTS.items():
        content = content.replace(old_include, new_include)
    
    # Special handling for main.cpp -> hymo_cli.cpp
    if filepath == 'main.cpp':
        content = transform_main_to_hymo_cli(content)
    
    return content

def transform_main_to_hymo_cli(content):
    """Transform standalone main.cpp to ksud hymo_cli.cpp (minimal changes)."""
    import re
    
    # Add sync header
    header = """// hymo_cli.cpp - HymoFS module management CLI
// Auto-synced from hymo/src/main.cpp
//
// Changes from standalone version:
// - int main() -> int hymo_main()
// - print_help() -> print_hymo_help()
// - "hymod" -> "ksud hymo" in help text
//
// NOTE: This file is auto-generated. Manual edits will be overwritten!

"""
    
    # 1. Replace main() with hymo_main()
    content = re.sub(
        r'\bint main\s*\(\s*int\s+argc\s*,\s*char\s*\*+\s*argv\[\]\s*\)',
        'int hymo_main(int argc, char** argv)',
        content
    )
    
    # 2. Replace print_help() function
    content = re.sub(
        r'\bstatic void print_help\(\)',
        'void print_hymo_help()',
        content
    )
    content = content.replace('print_help();', 'print_hymo_help();')
    
    # 3. Update help text
    content = content.replace('Usage: hymod', 'USAGE: ksud hymo')
    # Update command examples in help
    content = re.sub(
        r'(["\s])hymod ([a-z\-]+)',
        r'\1ksud hymo \2',
        content
    )
    
    # 4. Add cmd_hymo wrapper function at the end
    wrapper = """
namespace hymo {

// Wrapper function for ksud CLI integration
int cmd_hymo(const std::vector<std::string>& args) {
    // Convert args to argc/argv format
    std::vector<const char*> argv_ptrs;
    argv_ptrs.push_back("hymo");  // Program name
    for (const auto& arg : args) {
        argv_ptrs.push_back(arg.c_str());
    }
    
    int argc = static_cast<int>(argv_ptrs.size());
    char** argv = const_cast<char**>(argv_ptrs.data());
    
    return hymo_main(argc, argv);
}

}  // namespace hymo
"""
    
    return header + content + wrapper

def sync_file(src_path, dst_path):
    """Sync a single file with transformations."""
    src_file = HYMO_REPO_DIR / "src" / src_path
    dst_file = HYMO_EMBEDDED_DIR / dst_path
    
    if not src_file.exists():
        print(f"Warning: Source file not found: {src_file}", file=sys.stderr)
        return False
    
    # Read source file
    try:
        with open(src_file, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {src_file}: {e}", file=sys.stderr)
        return False
    
    # Transform content
    content = transform_code(content, str(src_path))
    
    # Create destination directory if needed
    dst_file.parent.mkdir(parents=True, exist_ok=True)
    
    # Write transformed content
    try:
        with open(dst_file, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"  ✓ Synced: {src_path} -> {dst_path}")
        return True
    except Exception as e:
        print(f"Error writing {dst_file}: {e}", file=sys.stderr)
        return False

def sync_all_files():
    """Sync all hymo source files."""
    print("\nSyncing hymo source files...")
    success_count = 0
    total_count = len(HYMO_SOURCE_MAPPINGS)
    
    for src_path, dst_path in HYMO_SOURCE_MAPPINGS.items():
        if sync_file(src_path, dst_path):
            success_count += 1
    
    print(f"\nSync complete: {success_count}/{total_count} files synced successfully")
    return success_count == total_count

def get_hymo_version():
    """Get hymo version from git."""
    version = run_command("git describe --tags --always", cwd=HYMO_REPO_DIR)
    if not version:
        version = run_command("git rev-parse --short HEAD", cwd=HYMO_REPO_DIR)
    return version or "unknown"

def update_version_info():
    """Update version info in hymo_defs.hpp."""
    version = get_hymo_version()
    print(f"\nHymo version: {version}")
    
    defs_file = HYMO_EMBEDDED_DIR / "hymo_defs.hpp"
    if not defs_file.exists():
        return
    
    # Add version comment at the top
    try:
        with open(defs_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # Add version comment if not present
        version_comment = f"// Synced from hymo {version}\n"
        if version_comment not in content:
            content = version_comment + content
            
            with open(defs_file, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"  ✓ Updated version info in hymo_defs.hpp")
    except Exception as e:
        print(f"Warning: Failed to update version info: {e}", file=sys.stderr)

def main():
    print("=" * 60)
    print("HymoFS Source Sync Script")
    print("=" * 60)
    
    # Step 1: Pull latest hymo code
    if not pull_hymo_repo():
        print("\nFailed to pull hymo repository", file=sys.stderr)
        return 1
    
    # Step 2: Sync all files
    if not sync_all_files():
        print("\nWarning: Some files failed to sync", file=sys.stderr)
        # Don't exit, continue with version update
    
    # Step 3: Update version info
    update_version_info()
    
    print("\n" + "=" * 60)
    print("Sync completed!")
    print("=" * 60)
    print("\nNext steps:")
    print("  1. Review the synced code for any breaking changes")
    print("  2. Test the build: cd userspace/ksud && mkdir -p build && cd build && cmake .. && make")
    print("  3. Commit the changes if everything works")
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
