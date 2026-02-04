# Third-party Libraries

This directory is managed by CMake FetchContent. Dependencies are automatically downloaded during the build process.

## Dependencies

### PicoSHA2

- **License**: MIT
- **Source**: https://github.com/okdshin/PicoSHA2
- **Version**: v1.0.1 (managed by FetchContent in CMakeLists.txt)
- **Purpose**: SHA-256 hash for partition / flash verification

### miniz

- **License**: Public domain (miniz)
- **Source**: https://github.com/richgel999/miniz
- **Version**: 3.0.2 (managed by FetchContent in CMakeLists.txt)
- **Purpose**: Lightweight ZIP library for boot image / kernel package handling

### Management

Dependencies are declared in `CMakeLists.txt` using `FetchContent_Declare`. To update a dependency version, modify the `GIT_TAG` in CMakeLists.txt. Dependabot may create PRs for version updates.

## Thanks

Thanks to the authors and maintainers of the above projects for making ksud possible.

