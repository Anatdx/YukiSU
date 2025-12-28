cmake_minimum_required(VERSION 3.14)
project(ksuinit VERSION 1.0.0 LANGUAGES CXX)

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
set(CMAKE_CXX_FLAGS_RELEASE "-Oz -DNDEBUG -flto")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -DNDEBUG -flto")
# 使用 lld 链接器加速链接过程 (特别是 LTO)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld -Wl,--gc-sections -Wl,--strip-all -flto")

add_executable(ksuinit
    src/main.cpp
    src/init.cpp
    src/loader.cpp
    src/log.cpp
)

target_include_directories(ksuinit PRIVATE src)

# No external dependencies - pure C++ with POSIX/Linux APIs
