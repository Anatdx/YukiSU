#!/bin/bash
# YukiSU libzygisk.so 最小化构建脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
OUTPUT_DIR="$SCRIPT_DIR/../../output/libzygisk"

# 支持的 ABI 列表
ABIS="${ABIS:-arm64-v8a armeabi-v7a}"

# 颜色输出
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== YukiSU libzygisk.so Minimal Build ===${NC}"

# 检查 NDK
if [ -z "$ANDROID_NDK_HOME" ]; then
    echo -e "${RED}Error: ANDROID_NDK_HOME not set${NC}"
    exit 1
fi

echo -e "${YELLOW}NDK: $ANDROID_NDK_HOME${NC}"
echo -e "${YELLOW}Building for ABIs: $ABIS${NC}"

mkdir -p "$OUTPUT_DIR"

# 为每个 ABI 构建
for ABI in $ABIS; do
    echo -e "\n${YELLOW}=== Building for $ABI ===${NC}"
    
    BUILD_ABI_DIR="$BUILD_DIR/$ABI"
    
    # 清理旧构建
    if [ -d "$BUILD_ABI_DIR" ]; then
        rm -rf "$BUILD_ABI_DIR"
    fi
    
    mkdir -p "$BUILD_ABI_DIR"
    cd "$BUILD_ABI_DIR"
    
    # 配置 CMake
    cmake ../.. \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$ABI \
        -DANDROID_PLATFORM=android-26 \
        -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release
    
    # 构建
    cmake --build . -j$(nproc)
    
    # 检查输出
    if [ -f "libzygisk.so" ]; then
        # 确定目标目录
        if [ "$ABI" = "arm64-v8a" ]; then
            TARGET_DIR="lib64"
        else
            TARGET_DIR="lib"
        fi
        
        # 复制到 output 和 ksud assets
        mkdir -p "$OUTPUT_DIR/$TARGET_DIR"
        cp libzygisk.so "$OUTPUT_DIR/$TARGET_DIR/libzygisk.so"
        
        SIZE=$(stat -c%s "libzygisk.so" 2>/dev/null || stat -f%z "libzygisk.so")
        SIZE_KB=$((SIZE / 1024))
        
        echo -e "${GREEN}✓ Built $ABI: ${SIZE_KB}KB${NC}"
    else
        echo -e "${RED}✗ Build failed for $ABI${NC}"
        exit 1
    fi
done

echo -e "\n${GREEN}=== Build Summary ===${NC}"
ls -lh "$OUTPUT_DIR"/*/*.so | awk '{print "  " $9 ": " $5}'

echo -e "\n${GREEN}Build complete!${NC}"
echo -e "${YELLOW}Note: Files automatically copied to ksud/assets/ for embedding${NC}"
