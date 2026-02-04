# Third-party Libraries

This directory is managed by CMake FetchContent. Dependencies are automatically downloaded during the build process.

## Dependencies

### miniz

- **License**: Public domain (miniz)
- **Source**: https://github.com/richgel999/miniz
- **Version**: 3.0.2 (managed by FetchContent in CMakeLists.txt)
- **Purpose**: Lightweight ZIP library for boot image / kernel package handling

### Management

Dependencies are declared in `CMakeLists.txt` using `FetchContent_Declare`. To update a dependency version, modify the `GIT_TAG` in CMakeLists.txt. Dependabot may create PRs for version updates.

