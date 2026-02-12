# Third-party Libraries

This directory contains **git submodules** and is also used by CMake **FetchContent** for some dependencies.

## Submodules

### MagiskbootAlone

- **Path**: `third_party/MagiskbootAlone`
- **Purpose**: Boot image unpack/repack/split-dtb; compiled **into ksud** as a multi-call binary.
- **Usage**: `ksu/bin/ksud` is the main binary; `ksu/bin/magiskboot` should be a **symlink to ksud**. When invoked as `magiskboot`, ksud dispatches by `argv[0]` to `magiskboot_main()`.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/MagiskbootAlone`

### bootctlAlone

- **Path**: `third_party/bootctlAlone`
- **Purpose**: `bootctl` binary (AOSP boot control); built in CI and copied to `assets/` for embedding into ksud.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/bootctlAlone`

### resetpropAlone

- **Path**: `third_party/resetpropAlone`
- **Purpose**: `resetprop` binary (getprop/setprop); built in CI and copied to `assets/` for embedding into ksud.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/resetpropAlone`

## FetchContent (auto-downloaded)

### PicoSHA2

- **License**: MIT
- **Source**: https://github.com/okdshin/PicoSHA2
- **Version**: v1.0.1 (managed by FetchContent in CMakeLists.txt)
- **Purpose**: SHA-256 hash calculation for APK signature verification

### Management

Dependencies are declared in `CMakeLists.txt` using `FetchContent_Declare`. 

To update a dependency version, modify the `GIT_TAG` in CMakeLists.txt:

```cmake
FetchContent_Declare(
    picosha2
    GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
    GIT_TAG v1.0.1  # Update this version
    GIT_SHALLOW TRUE
)
```

Dependabot will automatically detect and create PRs for version updates.

