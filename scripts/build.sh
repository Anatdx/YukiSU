#!/usr/bin/env bash
# YukiSU 本地构建: DDK LKM -> ksuinit -> ksud -> Manager App
# 签名环境变量: YUKISU_KEYSTORE, YUKISU_KEYSTORE_PASSWORD, YUKISU_KEY_ALIAS, YUKISU_KEY_PASSWORD
# 用法: ./scripts/build.sh [-k KMI] [-a ABI] [--skip-lkm] [-i] [-h]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$REPO_ROOT/out"
KMI="android15-6.6"
ABI="arm64-v8a"
SKIP_LKM=false
SKIP_HYMOFS=true
DDK_RELEASE="20251104"
DO_INSTALL=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    -k|--kmi) KMI="$2"; shift 2 ;;
    -a|--abi) ABI="$2"; shift 2 ;;
    --skip-lkm) SKIP_LKM=true; shift ;;
    --skip-hymofs) SKIP_HYMOFS=true; shift ;;
    --build-hymofs) SKIP_HYMOFS=false; shift ;;
    -i|--install) DO_INSTALL=true; shift ;;
    -h|--help)
      head -5 "$0" | tail -n +2 | sed 's/^# \?//'
      exit 0
      ;;
    *) echo "未知选项: $1"; exit 1 ;;
  esac
done

case "$ABI" in
  arm64-v8a)   TARGET_ARCH=aarch64; ANDROID_TARGET=aarch64-linux-android26 ;;
  x86_64)      TARGET_ARCH=x86_64;  ANDROID_TARGET=x86_64-linux-android26 ;;
  armeabi-v7a) TARGET_ARCH=armv7;   ANDROID_TARGET=armv7a-linux-androideabi26 ;;
  *) echo "不支持的 ABI: $ABI"; exit 1 ;;
esac

detect_ndk_host() {
  if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    echo "错误: 请设置 ANDROID_NDK_HOME"
    exit 1
  fi
  local prebuilt="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt"
  if [[ -d "$prebuilt/darwin-x86_64" ]]; then
    echo "darwin-x86_64"
  elif [[ -d "$prebuilt/darwin-arm64" ]]; then
    echo "darwin-arm64"
  elif [[ -d "$prebuilt/linux-x86_64" ]]; then
    echo "linux-x86_64"
  else
    echo "错误: 无法检测 NDK 预编译工具链，请检查 ANDROID_NDK_HOME"
    exit 1
  fi
}

NDK_HOST=$(detect_ndk_host)
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$NDK_HOST"

echo "=== YukiSU 本地构建 ==="
echo "KMI: $KMI | ABI: $ABI | NDK: $ANDROID_NDK_HOME"
echo ""

if [[ "$SKIP_LKM" != "true" ]]; then
  echo ">>> [1/5] 构建 KernelSU LKM (DDK) ..."
  mkdir -p "$OUT_DIR"
  docker run --rm -v "$REPO_ROOT:/src" -w /src \
    "ghcr.io/ylarod/ddk:${KMI}-${DDK_RELEASE}" \
    bash -c "cd kernel && CONFIG_KSU=m CONFIG_KSU_MANUAL_SU=y CONFIG_KSU_SUPERKEY=y CC=clang make -j$(nproc --all) && \
             mkdir -p /src/out && cp kernelsu.ko /src/out/${KMI}_kernelsu.ko && \
             llvm-strip -d /src/out/${KMI}_kernelsu.ko 2>/dev/null || true"
  echo "    LKM 已输出: $OUT_DIR/${KMI}_kernelsu.ko"
else
  echo ">>> [1/5] 跳过 LKM 构建"
fi

echo ">>> [2/5] 构建 ksuinit ..."
KSUINIT_DIR="$REPO_ROOT/userspace/ksuinit"
rm -rf "$KSUINIT_DIR/build"
mkdir -p "$KSUINIT_DIR/build"
cd "$KSUINIT_DIR/build"

export CC="$TOOLCHAIN/bin/${ANDROID_TARGET}-clang"
export CXX="$TOOLCHAIN/bin/${ANDROID_TARGET}-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"

cmake .. \
  -G Ninja \
  -DCMAKE_SYSTEM_NAME=Android \
  -DCMAKE_ANDROID_ARCH_ABI="$ABI" \
  -DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DKSUINIT_UPX=ON

ninja
echo "    ksuinit 已构建"

echo ">>> [3/5] 构建 ksud ..."
KSUD_ASSETS="$REPO_ROOT/userspace/ksud/assets"
mkdir -p "$KSUD_ASSETS"
mkdir -p "$OUT_DIR"

if [[ -f "$OUT_DIR/${KMI}_kernelsu.ko" ]]; then
  cp "$OUT_DIR/${KMI}_kernelsu.ko" "$KSUD_ASSETS/"
else
  echo "    警告: 未找到 ${KMI}_kernelsu.ko，ksud 可能无法正常加载内核模块"
fi

case "$TARGET_ARCH" in
  aarch64) arch_suffix="_arm64" ;;
  x86_64)  arch_suffix="_x86_64" ;;
  armv7)   arch_suffix="_armv7" ;;
  *)       arch_suffix="_arm64" ;;
esac
shopt -s nullglob 2>/dev/null || true
for d in "$OUT_DIR"/*-hymofs-lkm; do
  [[ -d "$d" ]] && cp "$d"/*"${arch_suffix}_hymofs_lkm.ko" "$KSUD_ASSETS/" 2>/dev/null || true
done

cp "$KSUINIT_DIR/build/ksuinit" "$KSUD_ASSETS/"

KSUD_DIR="$REPO_ROOT/userspace/ksud"
rm -rf "$KSUD_DIR/build"
mkdir -p "$KSUD_DIR/build"
cd "$KSUD_DIR/build"

cmake .. \
  -G Ninja \
  -DCMAKE_SYSTEM_NAME=Android \
  -DCMAKE_ANDROID_ARCH_ABI="$ABI" \
  -DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DKSUD_UPX=ON

ninja
echo "    ksud 已构建"

echo ">>> [4/5] 构建 Manager App ..."
MANAGER_DIR="$REPO_ROOT/manager"
JNILIBS="$MANAGER_DIR/app/src/main/jniLibs/$ABI"
mkdir -p "$JNILIBS"
cp "$KSUD_DIR/build/ksud" "$JNILIBS/libksud.so"

if [[ -n "${YUKISU_KEYSTORE:-}" && -n "${YUKISU_KEYSTORE_PASSWORD:-}" && -n "${YUKISU_KEY_ALIAS:-}" && -n "${YUKISU_KEY_PASSWORD:-}" ]]; then
  if [[ -f "$MANAGER_DIR/gradle.properties" ]]; then
    grep -v -E "^(KEYSTORE_FILE|KEYSTORE_PASSWORD|KEY_ALIAS|KEY_PASSWORD)=" "$MANAGER_DIR/gradle.properties" > "$MANAGER_DIR/gradle.properties.tmp" || true
    mv "$MANAGER_DIR/gradle.properties.tmp" "$MANAGER_DIR/gradle.properties"
  fi
  {
    echo ""
    echo "KEYSTORE_FILE=$YUKISU_KEYSTORE"
    echo "KEYSTORE_PASSWORD=$YUKISU_KEYSTORE_PASSWORD"
    echo "KEY_ALIAS=$YUKISU_KEY_ALIAS"
    echo "KEY_PASSWORD=$YUKISU_KEY_PASSWORD"
  } >> "$MANAGER_DIR/gradle.properties"
fi

cd "$MANAGER_DIR"
./gradlew assembleRelease --build-cache --no-daemon -PABI="$ABI"
echo "    APK 已构建"

APK_DIR="$MANAGER_DIR/app/build/outputs/apk/release"
echo ""
echo "=== 构建完成 ==="
echo "APK: $APK_DIR"
ls -la "$APK_DIR"/*.apk 2>/dev/null || true
echo ""

if [[ "$DO_INSTALL" == "true" ]]; then
  APK_FILE=$(ls "$APK_DIR"/*.apk 2>/dev/null | head -1)
  if [[ -n "$APK_FILE" ]]; then
    echo ">>> 安装到设备 ..."
    adb install -r "$APK_FILE" && echo "安装成功" || echo "安装失败"
  fi
else
  echo "安装命令: adb install -r $APK_DIR/*.apk"
  echo "或: ./scripts/build.sh --skip-lkm -i"
fi
