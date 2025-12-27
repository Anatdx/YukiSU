cmake_minimum_required(VERSION 3.14)
project(ksud VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 静态链接
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")

# 优化选项 (strip 由外部工具完成，不使用 -s 避免 clang 警告)
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")

# Generate embedded assets
set(ASSETS_DIR ${CMAKE_SOURCE_DIR}/assets)
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
set(ASSETS_CPP ${GENERATED_DIR}/assets_data.cpp)

# Create generated directory
file(MAKE_DIRECTORY ${GENERATED_DIR})

# Run embed_assets.py to generate assets_data.cpp
find_package(Python3 REQUIRED COMPONENTS Interpreter)
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
    src/core/susfs.cpp
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
    src/restorecon.cpp
    src/debug.cpp
    src/kpm.cpp
    ${ASSETS_CPP}
)

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${GENERATED_DIR})

add_executable(ksud ${SOURCES})

# 链接库 - Android bionic libc 已内置 pthread，不需要单独链接
if(NOT ANDROID)
    target_link_libraries(ksud PRIVATE pthread)
endif()

# 安装
install(TARGETS ksud DESTINATION bin)
