cmake_minimum_required(VERSION 3.14)
project(ksuinit VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use ccache if available
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

# Size optimization (Oz + LTO + UPX)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden -fno-rtti -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident -fno-stack-protector -faddrsig -fno-semantic-interposition")
set(CMAKE_CXX_FLAGS_RELEASE "-Oz -DNDEBUG -flto=full")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -DNDEBUG -flto=full")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld -flto=full -Wl,--gc-sections -Wl,--icf=all -Wl,--strip-all -Wl,--exclude-libs,ALL -Wl,-O2 -Wl,--lto-O3 -Wl,--hash-style=gnu -Wl,--build-id=none -Wl,-z,norelro")

add_executable(ksuinit
    src/main.cpp
    src/init.cpp
    src/loader.cpp
    src/log.cpp
)

target_include_directories(ksuinit PRIVATE src)

# Optional: UPX binary compression
option(KSUINIT_UPX "Use UPX to compress ksuinit binary" OFF)
if(KSUINIT_UPX)
    find_program(UPX_PROGRAM upx)
    if(UPX_PROGRAM)
        add_custom_command(TARGET ksuinit POST_BUILD
            COMMAND ${UPX_PROGRAM} --best --lzma $<TARGET_FILE:ksuinit>
            COMMENT "UPX: compressing ksuinit with LZMA (--best)..."
        )
        message(STATUS "UPX compression ENABLED (--best --lzma)")
    else()
        message(WARNING "KSUINIT_UPX=ON but upx not found in PATH; skipping UPX compression")
    endif()
endif()

# No external dependencies - pure C++ with POSIX/Linux APIs
