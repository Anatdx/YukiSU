#!/usr/bin/env python3
import subprocess
import sys
import os
import re


def normalize_version_name(describe):
    describe = describe.strip()
    match = re.match(r"^v?(\d+\.\d+\.\d+)(?:-\d+-g([0-9a-fA-F]+).*)?$", describe)
    if match:
        semver, commit = match.groups()
        return f"{semver}-{commit[:8]}" if commit else semver
    return describe.lstrip('v')

def get_git_version():
    try:
        count_output = subprocess.check_output(
            ["git", "rev-list", "--count", "HEAD"],
            stderr=subprocess.DEVNULL,
            text=True
        ).strip()
        version_code = int(count_output) + 10000
        
        try:
            tag_output = subprocess.check_output(
                ["git", "describe", "--tags", "--always", "--abbrev=8"],
                stderr=subprocess.DEVNULL,
                text=True
            ).strip()
            version_name = normalize_version_name(tag_output)
        except:
            version_name = subprocess.check_output(
                ["git", "rev-parse", "--short=8", "HEAD"],
                stderr=subprocess.DEVNULL,
                text=True
            ).strip()
        
        return version_code, version_name
    except:
        print("Warning: Failed to get git version, using defaults", file=sys.stderr)
        return 12000, "1.2.0"

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: generate_version.py <output_file>")
        sys.exit(1)
    
    output_file = sys.argv[1]
    code, name = get_git_version()
    
    # Generate C++ source file
    content = f'''#include "defs.hpp"

namespace ksud {{

// Auto-generated at build time
const char* const VERSION_CODE = "{code}";
const char* const VERSION_NAME = "{name}";

}}  // namespace ksud
'''
    
    # Only write if changed to avoid unnecessary rebuilds
    if os.path.exists(output_file):
        with open(output_file, 'r') as f:
            if f.read() == content:
                sys.exit(0)
    
    with open(output_file, 'w') as f:
        f.write(content)
    
    print(f"Generated version: {name} ({code})")
