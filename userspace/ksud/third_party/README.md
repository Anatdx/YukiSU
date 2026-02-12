# Third-party Libraries

ksud uses **git submodules** at the repo root under `MagiskToolsAlone/` and **FetchContent** for some dependencies (see CMakeLists.txt).

## Submodules (MagiskToolsAlone at repo root)

### MagiskbootAlone

- **Path**: `MagiskToolsAlone/MagiskbootAlone` (repo root)
- **Purpose**: Boot image unpack/repack/split-dtb; compiled **into ksud** as a multi-call binary.
- **Usage**: `ksu/bin/ksud` is the main binary; `ksu/bin/magiskboot` should be a **symlink to ksud**. When invoked as `magiskboot`, ksud dispatches by `argv[0]` to `magiskboot_main()`.
- **Init/update**: `git submodule update --init MagiskToolsAlone/MagiskbootAlone`

### bootctlAlone

- **Path**: `MagiskToolsAlone/bootctlAlone` (repo root)
- **Purpose**: Compiled **into ksud** as multi-call (like magiskboot). `ksu/bin/bootctl` → symlink to ksud; argv0 dispatch to `bootctl_main()`.
- **Init/update**: `git submodule update --init MagiskToolsAlone/bootctlAlone`

### resetpropAlone

- **Path**: `MagiskToolsAlone/resetpropAlone` (repo root)
- **Purpose**: Compiled **into ksud** as multi-call. `ksu/bin/resetprop` → symlink to ksud; argv0 dispatch to `resetprop_main()`.
- **Init/update**: `git submodule update --init MagiskToolsAlone/resetpropAlone`

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
