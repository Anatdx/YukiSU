# Prefer Ninja: cmake -G Ninja .. [options] && ninja
cmake_minimum_required(VERSION 3.14)

# Android minimum API 28 (getrandom/sethostname etc. require bionic symbols from API 28+)
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(CMAKE_ANDROID_API 28 CACHE STRING "Android API level (min 28)" FORCE)
endif()

# Prefer Clang for C and CXX (LTO, -faddrsig, etc. require Clang; do not use GCC)
if(NOT ANDROID AND NOT CMAKE_C_COMPILER)
  find_program(CLANG_CC NAMES clang-18 clang-17 clang-16 clang-15 clang-14 clang)
  find_program(CLANG_CXX NAMES clang++-18 clang++-17 clang++-16 clang++-15 clang++-14 clang++)
  if(CLANG_CC AND CLANG_CXX)
    set(CMAKE_C_COMPILER "${CLANG_CC}" CACHE STRING "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${CLANG_CXX}" CACHE STRING "CXX compiler" FORCE)
    message(STATUS "Using Clang: C=${CLANG_CC} CXX=${CLANG_CXX}")
  endif()
endif()

project(ksud VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android" OR
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "YukiSU userspace supports Android arm64-v8a only")
endif()

# Allow FetchContent deps (e.g. miniz) that use cmake_minimum_required < 3.5
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Min CMake version for subprojects")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use ccache to speed up builds (if available)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

# Backward-compatible alias for existing local invocations.
if(DEFINED KSUD_ENABLE_CLANG_TIDY AND
   NOT DEFINED YUKISU_ENABLE_CLANG_TIDY)
    set(YUKISU_ENABLE_CLANG_TIDY "${KSUD_ENABLE_CLANG_TIDY}"
        CACHE BOOL "Run clang-tidy on YukiSU first-party userspace sources")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ClangTidy.cmake")

# ============================================================================
# Super Aggressive Size Optimization
# ============================================================================
# Disable LTO if linker reports undefined symbols (e.g. ksud::boot_patch) in some
# environments (OrbStack host build with object libs + LTO). Use: -DKSUD_DISABLE_LTO=ON
option(KSUD_DISABLE_LTO "Disable LTO (use if link fails with undefined symbols)" OFF)

# --- Common compile flags (shared by C and C++) ---
set(_COMMON_COMPILE_FLAGS
    -ffunction-sections          # Place each function in its own section (enables --gc-sections dead code elimination)
    -fdata-sections              # Place each data item in its own section
    -fvisibility=hidden          # Hide all symbols by default, shrink export table
    -fvisibility-inlines-hidden  # Hide inline function symbols
    -fno-rtti                    # Disable RTTI (no dynamic_cast/typeid usage in codebase)
    -fno-asynchronous-unwind-tables  # Remove async unwind tables (C++ exceptions still work)
    -fmerge-all-constants        # Merge identical constants across translation units
    -fno-ident                   # Don't embed compiler version string into .comment section
    -fno-stack-protector         # Remove stack canary checks (unnecessary for root daemon)
    -faddrsig                    # Emit address-significance table for more precise ICF
    -fno-semantic-interposition  # Disable semantic interposition, allow more aggressive inlining
    -fno-math-errno              # Math functions don't set errno
)
string(REPLACE ";" " " _COMMON_FLAGS_STR "${_COMMON_COMPILE_FLAGS}")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${_COMMON_FLAGS_STR}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_COMMON_FLAGS_STR}")

# --- Release build optimization ---
if(KSUD_DISABLE_LTO)
    set(CMAKE_C_FLAGS_RELEASE   "-Os -DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")
else()
    set(CMAKE_C_FLAGS_RELEASE   "-Os -DNDEBUG -flto=full")
    set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG -flto=full")
endif()

# --- Aggressive linker optimization ---
if(KSUD_DISABLE_LTO)
    string(CONCAT _LINKER_FLAGS
        " -fuse-ld=lld"
        " -Wl,--gc-sections"
        " -Wl,--strip-all"
        " -Wl,--exclude-libs,ALL"
        " -Wl,-O2"
        " -Wl,--hash-style=gnu"
        " -Wl,--build-id=none"
        " -Wl,-z,norelro"
    )
else()
    string(CONCAT _LINKER_FLAGS
        " -fuse-ld=lld"               # Use lld linker
        " -flto=full"                 # Full LTO: maximum cross-module optimization (smaller than thin LTO)
        " -Wl,--gc-sections"          # Garbage-collect unused sections
        " -Wl,--icf=all"              # Identical Code Folding: merge functions with identical bodies
        " -Wl,--strip-all"            # Strip all symbols
        " -Wl,--exclude-libs,ALL"     # Don't export symbols from static libraries
        " -Wl,-O2"                    # Linker optimization level 2 (string table optimization etc.)
        " -Wl,--lto-O3"              # LTO at O3 level (compile with Oz, but LTO pass uses O3 for aggressive inlining/elimination)
        " -Wl,--hash-style=gnu"       # GNU hash is more compact than sysv
        " -Wl,--build-id=none"        # Remove build-id note section
        " -Wl,-z,norelro"             # Remove RELRO segment (root tool doesn't need this security feature)
    )
endif()
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}${_LINKER_FLAGS}")

# --- Shrink miniz: disable unused features ---
add_compile_definitions(
    MINIZ_NO_STDIO               # No FILE* interface needed
    MINIZ_NO_TIME                 # No timestamp support needed
    MINIZ_NO_ARCHIVE_WRITING_APIS # Decompression only, disable write APIs
)

# Fetch third-party dependencies
include(FetchContent)

# mbedTLS - SHA1/SHA256 + X.509/RSA/ECDSA (for MagiskbootAlone AVB and flash_partition SHA256)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)  # avoid -Werror vs -fno-semantic-interposition on Apple Clang
FetchContent_Declare(
    mbedtls
    # The official release archive includes the framework submodule and generated files.
    # GitHub's automatic source archives and a plain shallow clone do not.
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
    URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
)
FetchContent_MakeAvailable(mbedtls)

# miniz - Lightweight ZIP library
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniz)

# LZ4 - prefer Magisk vendored inside MagiskbootAlone submodule (same ABI as Magisk magiskboot)
set(LZ4_IN_MAGISKBOOT_ALONE ${CMAKE_SOURCE_DIR}/third_party/MagiskbootAlone/external/lz4)
if(EXISTS ${LZ4_IN_MAGISKBOOT_ALONE}/lz4.c)
    set(LZ4_LIB_DIR ${LZ4_IN_MAGISKBOOT_ALONE})
    message(STATUS "Using vendored LZ4 (MagiskbootAlone/external/lz4) for magiskboot: ${LZ4_LIB_DIR}")
else()
    FetchContent_Declare(
        lz4
        GIT_REPOSITORY https://github.com/lz4/lz4.git
        GIT_TAG v1.10.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(lz4)
    set(LZ4_LIB_DIR ${lz4_SOURCE_DIR}/lib)
endif()

# Generated sources / embedded assets
set(ASSETS_DIR ${CMAKE_SOURCE_DIR}/assets)
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
set(ASSETS_CPP ${GENERATED_DIR}/assets_data.cpp)
set(VERSION_CPP ${CMAKE_SOURCE_DIR}/src/defs.cpp)
file(MAKE_DIRECTORY ${GENERATED_DIR})

# Generate version file at configure time
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/generate_version.py ${VERSION_CPP}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE VERSION_RESULT
)
if(NOT VERSION_RESULT EQUAL 0)
    message(WARNING "Failed to generate version file, using defaults")
endif()

# Run embed_assets.py to generate assets_data.cpp from everything in assets/. All
# assets (ko, ksuinit, su) are staged into assets/ by build.sh BEFORE configure,
# so the glob catches them. DEPEND on the asset files (not just the script) plus
# CONFIGURE_DEPENDS so the table regenerates whenever an asset is added/changed.
file(GLOB ASSET_FILES CONFIGURE_DEPENDS ${ASSETS_DIR}/*)
add_custom_command(
    OUTPUT ${ASSETS_CPP}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_assets.py
            ${ASSETS_DIR} ${ASSETS_CPP}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/embed_assets.py ${ASSET_FILES}
    COMMENT "Generating embedded assets..."
)

# third_party tools: compile into ksud for multi-call
set(MAGISKTOOLS_ALONE_DIR ${CMAKE_SOURCE_DIR}/third_party)
set(MAGISKBOOT_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/MagiskbootAlone)
set(MAGISKBOOT_SOURCES
    ${MAGISKBOOT_ALONE_DIR}/src/base_host.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/boot_ramdisk.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/bootimg.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/boot_crypto.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/cpio.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/magiskboot_main.cpp
    ${LZ4_LIB_DIR}/lz4.c
    ${LZ4_LIB_DIR}/lz4frame.c
    ${LZ4_LIB_DIR}/lz4hc.c
    ${LZ4_LIB_DIR}/xxhash.c
)
if(EXISTS ${MAGISKBOOT_ALONE_DIR}/src/magiskboot_main.cpp)
    include_directories(${MAGISKBOOT_ALONE_DIR}/src ${LZ4_LIB_DIR})
    set(MAGISKBOOT_ALONE_AVAILABLE 1)
    message(STATUS "MagiskbootAlone: building magiskboot into ksud (multi-call)")
else()
    message(WARNING "MagiskbootAlone submodule not found at ${MAGISKBOOT_ALONE_DIR}; run: git submodule update --init")
endif()

set(BOOTCTL_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/bootctlAlone)
set(RESETPROP_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/resetpropAlone)
set(MKBOOTFS_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/mkbootfsAlone)
set(RESETPROP_ALONE_SOURCES)
if(EXISTS ${BOOTCTL_ALONE_DIR}/src/main.cpp)
    set(BOOTCTL_ALONE_AVAILABLE 1)
    message(STATUS "bootctlAlone: building bootctl into ksud (multi-call)")
endif()
if(EXISTS ${RESETPROP_ALONE_DIR}/src/main.cpp)
    file(GLOB RESETPROP_ALONE_SOURCES CONFIGURE_DEPENDS
        ${RESETPROP_ALONE_DIR}/src/*.cpp
    )
    set(RESETPROP_ALONE_AVAILABLE 1)
    message(STATUS "resetpropAlone: building resetprop into ksud (multi-call)")
endif()
if(EXISTS ${MKBOOTFS_ALONE_DIR}/src/mkbootfs.cpp)
    set(MKBOOTFS_ALONE_AVAILABLE 1)
    message(STATUS "mkbootfsAlone: building mkbootfs into ksud (multi-call)")
endif()

# ndk-busybox: build as static lib and link into ksud for multi-call (argv0 "busybox")
set(NDK_BUSYBOX_DIR ${MAGISKTOOLS_ALONE_DIR}/ndk-busybox)
if(EXISTS ${NDK_BUSYBOX_DIR}/CMakeLists.txt)
  set(BUSYBOX_EMBEDDED 1 CACHE BOOL "Build busybox as static lib for embedding")
  add_subdirectory(${NDK_BUSYBOX_DIR} ${CMAKE_BINARY_DIR}/ndk-busybox)
  set(NDK_BUSYBOX_AVAILABLE 1)
  message(STATUS "ndk-busybox: building busybox into ksud (multi-call)")
endif()

# Our own sources (clang-tidy will run on these only)
set(KSUD_OUR_SOURCES
    src/main.cpp
    src/cli.cpp
    src/defs.cpp
    src/log.cpp
    src/utils.cpp
    src/kernelsu_loader.cpp
    src/dynamic_manager.cpp
    src/core/ksucalls.cpp
    src/core/feature.cpp
    src/core/restorecon.cpp
    src/core/assets.cpp
    src/module/module.cpp
    src/module/module_config.cpp
    src/module/metamodule.cpp
    src/boot/boot_patch.cpp
    src/boot/ramdisk_editor.cpp
    src/boot/tools.cpp
    src/boot/apk_sign.cpp
    src/profile/profile.cpp
    src/sepolicy/sepolicy.cpp
    src/su.cpp
    src/init_event.cpp
    src/yukizygisk_snapshot.cpp
    src/late_load.cpp
    src/umount.cpp
    src/debug.cpp
    src/sulog.cpp
    src/magisk_compat/msud.cpp
    src/magisk_compat/su_mount.cpp
    src/magisk_compat/su_magic.cpp
    src/magisk_compat/su_protocol.cpp
    src/magica/adb_client.cpp
    src/magica/magica.cpp
    src/core/hide_bootloader.cpp
    src/flash/flash_ak3.cpp
    src/flash/flash_partition.cpp
)

# Third-party sources (no clang-tidy)
set(KSUD_THIRDPARTY_SOURCES)
if(MAGISKBOOT_ALONE_AVAILABLE)
    list(APPEND KSUD_THIRDPARTY_SOURCES ${MAGISKBOOT_SOURCES})
endif()
if(BOOTCTL_ALONE_AVAILABLE)
    list(APPEND KSUD_THIRDPARTY_SOURCES ${BOOTCTL_ALONE_DIR}/src/main.cpp)
endif()
if(RESETPROP_ALONE_AVAILABLE)
    list(APPEND KSUD_THIRDPARTY_SOURCES ${RESETPROP_ALONE_SOURCES})
endif()
if(MKBOOTFS_ALONE_AVAILABLE)
    list(APPEND KSUD_THIRDPARTY_SOURCES ${MKBOOTFS_ALONE_DIR}/src/mkbootfs.cpp)
endif()

get_filename_component(REPO_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)

# Zygisk daemon (zygiskd), welded into ksud as a multi-call face. Invoked as
# `ksud zygiskd` or via a `zygiskd` -> ksud symlink. It is first-party
# userspace code and gets its own checked object target below.
set(ZYGISKD_DIR ${REPO_ROOT}/userspace/zygisk/daemon)
if(EXISTS ${ZYGISKD_DIR}/zygiskd.cpp)
    set(ZYGISKD_AVAILABLE 1)
endif()

# Include directories (for both object libs and ksud)
set(KSUD_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
    ${mbedtls_SOURCE_DIR}/include
    ${miniz_SOURCE_DIR}
    ${miniz_BINARY_DIR}
    ${LZ4_LIB_DIR}
    ${MKBOOTFS_ALONE_DIR}/include
    ${GENERATED_DIR}
    ${REPO_ROOT}  # uapi/ kernel headers for userspace
)
include_directories(${KSUD_INCLUDE_DIRS})

# Compile definitions shared by our and third-party object libs
set(KSUD_DEFINES)
if(MAGISKBOOT_ALONE_AVAILABLE)
    list(APPEND KSUD_DEFINES MAGISKBOOT_ALONE_AVAILABLE=1 USE_MBEDTLS=1)
endif()
if(BOOTCTL_ALONE_AVAILABLE)
    list(APPEND KSUD_DEFINES BOOTCTL_ALONE_AVAILABLE=1)
endif()
if(RESETPROP_ALONE_AVAILABLE)
    list(APPEND KSUD_DEFINES RESETPROP_ALONE_AVAILABLE=1)
endif()
if(MKBOOTFS_ALONE_AVAILABLE)
    list(APPEND KSUD_DEFINES
        MKBOOTFS_ALONE_AVAILABLE=1
        MKBOOTFS_ALONE_VERSION="1.0.0"
        MKBOOTFS_AOSP_REVISION="a3b721a32242006b59cb12bd62c9133632af3a2d"
    )
endif()
if(NDK_BUSYBOX_AVAILABLE)
    list(APPEND KSUD_DEFINES NDK_BUSYBOX_AVAILABLE=1)
endif()
if(ZYGISKD_AVAILABLE)
    list(APPEND KSUD_DEFINES ZYGISKD_AVAILABLE=1)
endif()

# Object library: our sources (with clang-tidy when enabled). The Android NDK
# compilation database supplies the real target sysroot and flags to clang-tidy.
add_library(ksud_our_objs OBJECT ${KSUD_OUR_SOURCES})
target_include_directories(ksud_our_objs PRIVATE ${KSUD_INCLUDE_DIRS})
target_compile_definitions(ksud_our_objs PRIVATE ${KSUD_DEFINES})
target_compile_features(ksud_our_objs PUBLIC cxx_std_17)
yukisu_enable_clang_tidy(ksud_our_objs CXX)

set(KSUD_FIRSTPARTY_EXTRA_OBJECTS)
if(ZYGISKD_AVAILABLE)
    add_library(ksud_zygiskd_objs OBJECT ${ZYGISKD_DIR}/zygiskd.cpp)
    target_include_directories(ksud_zygiskd_objs PRIVATE ${KSUD_INCLUDE_DIRS})
    target_compile_definitions(ksud_zygiskd_objs PRIVATE ${KSUD_DEFINES})
    target_compile_features(ksud_zygiskd_objs PUBLIC cxx_std_20)
    set(_KSUD_CLANG_TIDY_CONFIG "${YUKISU_CLANG_TIDY_CONFIG}")
    set(YUKISU_CLANG_TIDY_CONFIG "${ZYGISKD_DIR}/../.clang-tidy")
    yukisu_enable_clang_tidy(ksud_zygiskd_objs CXX)
    set(YUKISU_CLANG_TIDY_CONFIG "${_KSUD_CLANG_TIDY_CONFIG}")
    list(APPEND KSUD_FIRSTPARTY_EXTRA_OBJECTS
        $<TARGET_OBJECTS:ksud_zygiskd_objs>)
endif()

# Generated compressed asset bytes are compiled into ksud, but are not useful
# clang-tidy input. Keep them separate from the hand-written first-party code.
add_library(ksud_generated_objs OBJECT ${ASSETS_CPP})
target_include_directories(ksud_generated_objs PRIVATE ${KSUD_INCLUDE_DIRS})
target_compile_definitions(ksud_generated_objs PRIVATE ${KSUD_DEFINES})

# Object library: third-party sources (no clang-tidy)
add_library(ksud_thirdparty_objs OBJECT ${KSUD_THIRDPARTY_SOURCES})
target_include_directories(ksud_thirdparty_objs PRIVATE ${KSUD_INCLUDE_DIRS})
target_compile_definitions(ksud_thirdparty_objs PRIVATE ${KSUD_DEFINES})
if(MAGISKBOOT_ALONE_AVAILABLE)
    target_compile_features(ksud_thirdparty_objs PUBLIC cxx_std_17)
endif()

# Executable: link object libs
add_executable(ksud
    $<TARGET_OBJECTS:ksud_our_objs>
    ${KSUD_FIRSTPARTY_EXTRA_OBJECTS}
    $<TARGET_OBJECTS:ksud_generated_objs>
    $<TARGET_OBJECTS:ksud_thirdparty_objs>
)
target_compile_definitions(ksud PRIVATE ${KSUD_DEFINES})
if(ZYGISKD_AVAILABLE)
    # zygiskd (welded in) logs via liblog's __android_log_print
    target_link_libraries(ksud PRIVATE log)
endif()
if(NDK_BUSYBOX_AVAILABLE)
    target_compile_definitions(ksud PRIVATE NDK_BUSYBOX_AVAILABLE=1)
    target_link_libraries(ksud PRIVATE busybox)
    # libselinux not in NDK; link only when found (e.g. host or custom Android sysroot)
    find_library(SELINUX_LIB selinux)
    if(SELINUX_LIB)
      target_link_libraries(ksud PRIVATE selinux)
    endif()
    # busybox expects these wraps (from Android.mk)
    target_link_options(ksud PRIVATE -Wl,--wrap=realpath -Wl,--wrap=rename -Wl,--wrap=renameat)
endif()

# Link libraries - Android bionic libc has built-in pthread, no separate linking needed
if(NOT ANDROID)
    target_link_libraries(ksud PRIVATE pthread)
endif()

target_link_libraries(ksud PRIVATE z miniz mbedx509 mbedcrypto dl)

# ============================================================================
# Post-build Size Compression
# ============================================================================

# llvm-strip: more precise than --strip-all, removes .comment / .note misc sections
find_program(LLVM_STRIP llvm-strip)
if(LLVM_STRIP)
    add_custom_command(TARGET ksud POST_BUILD
        COMMAND ${LLVM_STRIP}
            --strip-all                  # Remove all symbols
            --remove-section=.comment    # Remove compiler version info
            --remove-section=.note       # Remove note section
            --remove-section=.note.android.ident
            --remove-section=.note.gnu.build-id
            $<TARGET_FILE:ksud>
        COMMENT "llvm-strip: removing all debug info & note sections..."
    )
endif()

# Optional: UPX binary compression (runtime self-extracting, typically 50-70% further reduction)
# Pass -DKSUD_UPX=ON to enable; off by default as some environments don't support UPX
option(KSUD_UPX "Use UPX to compress ksud binary" OFF)
if(KSUD_UPX)
    find_program(UPX_PROGRAM upx)
    if(UPX_PROGRAM)
        add_custom_command(TARGET ksud POST_BUILD
            COMMAND ${UPX_PROGRAM} --best --lzma $<TARGET_FILE:ksud>
            COMMENT "UPX: compressing ksud with LZMA (--best)..."
        )
        message(STATUS "UPX compression ENABLED (--best --lzma)")
    else()
        message(WARNING "KSUD_UPX=ON but upx not found in PATH; skipping UPX compression")
    endif()
endif()

# Report the final artifact. `ls` is not guaranteed to exist for a native
# Windows/Ninja build, while it remains useful for the Unix build script.
if(CMAKE_HOST_WIN32)
    add_custom_command(TARGET ksud POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "-- ksud built: $<TARGET_FILE:ksud>"
        COMMENT "Reporting final ksud artifact..."
    )
else()
    add_custom_command(TARGET ksud POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "-- ksud final size:"
        COMMAND ls -lh $<TARGET_FILE:ksud>
        COMMENT "Reporting final binary size..."
    )
endif()

# Install
install(TARGETS ksud DESTINATION bin)

# NOTE: the standalone magisk-compat `su` is built separately (userspace/su,
# like ksuinit) and staged into assets/ by build.sh BEFORE this CMake configures,
# so it is embedded as an ordinary prebuilt asset -- ksud does NOT compile it.
