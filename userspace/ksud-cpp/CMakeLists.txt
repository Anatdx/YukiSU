cmake_minimum_required(VERSION 3.14)
project(ksud VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 静态链接
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")

# 优化选项
set(CMAKE_CXX_FLAGS_RELEASE "-Os -s -DNDEBUG")

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
    src/assets.cpp
)

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/src)

add_executable(ksud ${SOURCES})

# 链接库
target_link_libraries(ksud PRIVATE
    pthread
)

# 安装
install(TARGETS ksud DESTINATION bin)
