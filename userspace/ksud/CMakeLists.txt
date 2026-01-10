cmake_minimum_required(VERSION 3.14)
project(ksud VERSION 1.0.0 LANGUAGES CXX)

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
set(CMAKE_CXX_FLAGS_RELEASE "-Oz -DNDEBUG -flto")

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

# Generate embedded assets
set(ASSETS_DIR ${CMAKE_SOURCE_DIR}/assets)
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
set(ASSETS_CPP ${GENERATED_DIR}/assets_data.cpp)
set(VERSION_CPP ${CMAKE_SOURCE_DIR}/src/defs.cpp)

# Create generated directory
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

# Run embed_assets.py to generate assets_data.cpp
add_custom_command(
    OUTPUT ${ASSETS_CPP}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_assets.py 
            ${ASSETS_DIR} ${ASSETS_CPP}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/embed_assets.py
    COMMENT "Generating embedded assets..."
)

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
    src/boot/apk_sign.cpp
    src/profile/profile.cpp
    src/sepolicy/sepolicy.cpp
    src/su.cpp
    src/init_event.cpp
    src/umount.cpp
    src/debug.cpp
    src/kpm.cpp
    src/property_monitor.cpp
    # Core features
    src/core/hide_bootloader.cpp
    # Flash kernel packages
    src/flash/flash_ak3.cpp
    # HymoFS module management
    src/hymo/hymo_cli.cpp
    src/hymo/hymo_utils.cpp
    src/hymo/conf/config.cpp
    src/hymo/core/executor.cpp
    src/hymo/core/inventory.cpp
    src/hymo/core/modules.cpp
    src/hymo/core/planner.cpp
    src/hymo/core/state.cpp
    src/hymo/core/storage.cpp
    src/hymo/core/sync.cpp
    src/hymo/mount/hymofs.cpp
    src/hymo/mount/magic.cpp
    src/hymo/mount/overlay.cpp
    # Murasaki Binder service
    src/binder/murasaki_binder.cpp
    src/binder/murasaki_service.cpp
    src/binder/murasaki_ipc.cpp
    # Shizuku compatible service
    src/binder/shizuku_service.cpp
    # Zygisk kernel support
    src/zygisk/zygisk.cpp
    src/zygisk/ptracer/injector.cpp
    src/zygisk/ptracer/ptrace_utils.cpp
    ${ASSETS_CPP}
)

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${picosha2_SOURCE_DIR})
include_directories(${miniz_SOURCE_DIR})
include_directories(${GENERATED_DIR})

add_executable(ksud ${SOURCES})

# 链接库 - Android bionic libc 已内置 pthread，不需要单独链接
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(ksud PRIVATE pthread)
endif()

# libbinder_ndk headers and dl library for dynamic loading (Android only)
# We use dlopen() to load libbinder_ndk.so at runtime, so no direct linking needed
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(ksud PRIVATE dl)
endif()

target_link_libraries(ksud PRIVATE z miniz)

# 安装
install(TARGETS ksud DESTINATION bin)
