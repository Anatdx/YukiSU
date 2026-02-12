cmake_minimum_required(VERSION 3.14)
project(ksud VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 使用 ccache 加速编译 (如果可用)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

# 极限体积优化
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -fvisibility=hidden")
# 使用 lld 链接器加速链接过程 (特别是 LTO)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld -Wl,--gc-sections -Wl,--strip-all -flto")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG -flto")

# Fetch third-party dependencies
include(FetchContent)

# PicoSHA2 - SHA-256 implementation
FetchContent_Declare(
    picosha2
    GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
    GIT_TAG v1.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(picosha2)

# miniz - Lightweight ZIP library
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniz)

set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
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

# MagiskToolsAlone/* (submodules at repo root): compile into ksud for multi-call (magiskboot / bootctl / resetprop via argv0)
set(MAGISKTOOLS_ALONE_DIR ${CMAKE_SOURCE_DIR}/../../MagiskToolsAlone)
set(MAGISKBOOT_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/MagiskbootAlone)
set(MAGISKBOOT_SOURCES
    ${MAGISKBOOT_ALONE_DIR}/src/base_host.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/bootimg.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/boot_crypto.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/magiskboot_main.cpp
)
if(EXISTS ${MAGISKBOOT_ALONE_DIR}/src/magiskboot_main.cpp)
    set_source_files_properties(${MAGISKBOOT_SOURCES} PROPERTIES CXX_STANDARD 20)
    include_directories(${MAGISKBOOT_ALONE_DIR}/src)
    set(MAGISKBOOT_ALONE_AVAILABLE 1)
    message(STATUS "MagiskbootAlone: building magiskboot into ksud (multi-call)")
else()
    message(WARNING "MagiskbootAlone submodule not found at ${MAGISKBOOT_ALONE_DIR}; run: git submodule update --init")
endif()

set(BOOTCTL_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/bootctlAlone)
set(RESETPROP_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/resetpropAlone)
if(EXISTS ${BOOTCTL_ALONE_DIR}/src/main.cpp)
    set(BOOTCTL_ALONE_AVAILABLE 1)
    message(STATUS "bootctlAlone: building bootctl into ksud (multi-call)")
endif()
if(EXISTS ${RESETPROP_ALONE_DIR}/src/main.cpp)
    set(RESETPROP_ALONE_AVAILABLE 1)
    message(STATUS "resetpropAlone: building resetprop into ksud (multi-call)")
endif()

# ndk-busybox: build as static lib and link into ksud for multi-call (argv0 "busybox")
set(NDK_BUSYBOX_DIR ${MAGISKTOOLS_ALONE_DIR}/ndk-busybox)
if(EXISTS ${NDK_BUSYBOX_DIR}/CMakeLists.txt)
  set(BUSYBOX_EMBEDDED 1 CACHE BOOL "Build busybox as static lib for embedding")
  add_subdirectory(${NDK_BUSYBOX_DIR} ${CMAKE_BINARY_DIR}/ndk-busybox)
  set(NDK_BUSYBOX_AVAILABLE 1)
  message(STATUS "ndk-busybox: building busybox into ksud (multi-call)")
endif()

# 源文件
set(SOURCES
    src/main.cpp
    src/cli.cpp
    src/defs.cpp
    src/log.cpp
    src/utils.cpp
    src/core/ksucalls.cpp
    src/core/feature.cpp
    src/core/restorecon.cpp
    src/core/assets.cpp
    src/module/module.cpp
    src/module/module_config.cpp
    src/module/metamodule.cpp
    src/boot/boot_patch.cpp
    src/boot/tools.cpp
    src/boot/apk_sign.cpp
    src/profile/profile.cpp
    src/sepolicy/sepolicy.cpp
    src/su.cpp
    src/init_event.cpp
    src/umount.cpp
    src/debug.cpp
    # Core features
    src/core/hide_bootloader.cpp
    # Flash kernel packages
    src/flash/flash_partition.cpp
    # HymoFS module management (synced with meta-hymo hymod)
    src/hymo/hymo_cli.cpp
    src/hymo/hymo_main.cpp
    src/hymo/utils.cpp
    src/hymo/conf/config.cpp
    src/hymo/core/executor.cpp
    src/hymo/core/inventory.cpp
    src/hymo/core/modules.cpp
    src/hymo/core/planner.cpp
    src/hymo/core/state.cpp
    src/hymo/core/storage.cpp
    src/hymo/core/sync.cpp
    src/hymo/core/user_rules.cpp
    src/hymo/core/webui.cpp
    src/hymo/mount/hymofs.cpp
    src/hymo/mount/magic.cpp
    src/hymo/mount/overlay.cpp
    src/hymo/mount/mount_utils.cpp
    src/hymo/mount/partition_utils.cpp
)
if(MAGISKBOOT_ALONE_AVAILABLE)
    list(APPEND SOURCES ${MAGISKBOOT_SOURCES})
endif()
if(BOOTCTL_ALONE_AVAILABLE)
    list(APPEND SOURCES ${BOOTCTL_ALONE_DIR}/src/main.cpp)
endif()
if(RESETPROP_ALONE_AVAILABLE)
    list(APPEND SOURCES ${RESETPROP_ALONE_DIR}/src/main.cpp)
endif()

# 头文件目录 (src/hymo for hymo/*.cpp includes like "conf/config.hpp", "defs.hpp")
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/src/hymo)
include_directories(${picosha2_SOURCE_DIR})
include_directories(${miniz_SOURCE_DIR})
include_directories(${GENERATED_DIR})

add_executable(ksud ${SOURCES})

if(MAGISKBOOT_ALONE_AVAILABLE)
    target_compile_definitions(ksud PRIVATE MAGISKBOOT_ALONE_AVAILABLE=1)
endif()
if(BOOTCTL_ALONE_AVAILABLE)
    target_compile_definitions(ksud PRIVATE BOOTCTL_ALONE_AVAILABLE=1)
endif()
if(RESETPROP_ALONE_AVAILABLE)
    target_compile_definitions(ksud PRIVATE RESETPROP_ALONE_AVAILABLE=1)
endif()
if(NDK_BUSYBOX_AVAILABLE)
    target_compile_definitions(ksud PRIVATE NDK_BUSYBOX_AVAILABLE=1)
    target_link_libraries(ksud PRIVATE busybox)
    if(CMAKE_SYSTEM_NAME STREQUAL "Android")
      target_link_libraries(ksud PRIVATE selinux)
    endif()
    # busybox expects these wraps (from Android.mk)
    target_link_options(ksud PRIVATE -Wl,--wrap=realpath -Wl,--wrap=rename -Wl,--wrap=renameat)
endif()

# 链接库 - Android bionic libc 已内置 pthread，不需要单独链接
if(NOT ANDROID)
    target_link_libraries(ksud PRIVATE pthread)
endif()

target_link_libraries(ksud PRIVATE z miniz)

# 安装
install(TARGETS ksud DESTINATION bin)
