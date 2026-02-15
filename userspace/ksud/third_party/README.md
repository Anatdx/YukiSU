# Third-party Libraries

ksud uses **git submodules** in this directory (`userspace/ksud/third_party/`) and **FetchContent** for some dependencies (see ../CMakeLists.txt).

## Submodules (this directory)

### MagiskbootAlone

- **Path**: `userspace/ksud/third_party/MagiskbootAlone`
- **Purpose**: Boot image unpack/repack/split-dtb; compiled **into ksud** as a multi-call binary.
- **Usage**: `ksu/bin/ksud` is the main binary; `ksu/bin/magiskboot` should be a **symlink to ksud**. When invoked as `magiskboot`, ksud dispatches by `argv[0]` to `magiskboot_main()`.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/MagiskbootAlone`

### bootctlAlone

- **Path**: `userspace/ksud/third_party/bootctlAlone`
- **Purpose**: Compiled **into ksud** as multi-call (like magiskboot). `ksu/bin/bootctl` → symlink to ksud; argv0 dispatch to `bootctl_main()`.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/bootctlAlone`

### resetpropAlone

- **Path**: `userspace/ksud/third_party/resetpropAlone`
- **Purpose**: Compiled **into ksud** as multi-call. `ksu/bin/resetprop` → symlink to ksud; argv0 dispatch to `resetprop_main()`.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/resetpropAlone`

### ndk-busybox

- **Path**: `userspace/ksud/third_party/ndk-busybox`
- **Purpose**: Compiled **into ksud** as multi-call. `ksu/bin/busybox` → symlink to ksud; argv0 dispatch to `busybox_main()`.
- **Init/update**: `git submodule update --init userspace/ksud/third_party/ndk-busybox`

## FetchContent (auto-downloaded)

### PicoSHA2

- **License**: MIT
- **Source**: https://github.com/okdshin/PicoSHA2
- **Version**: v1.0.1 (managed by FetchContent in CMakeLists.txt)
- **Purpose**: SHA-256 hash calculation for APK signature verification

### Management

Dependencies are declared in `../CMakeLists.txt` using `FetchContent_Declare`.

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
