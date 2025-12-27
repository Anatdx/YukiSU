cmake_minimum_required(VERSION 3.14)
project(ksuinit VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static linking for standalone binary
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")

# Optimize for size
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG")

# Strip debug symbols in release
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -s")

add_executable(ksuinit
    src/main.cpp
    src/init.cpp
    src/loader.cpp
    src/log.cpp
)

target_include_directories(ksuinit PRIVATE src)

# No external dependencies - pure C++ with POSIX/Linux APIs
