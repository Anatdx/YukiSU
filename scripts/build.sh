#!/usr/bin/env bash
# YukiSU local build: DDK LKM -> ksuinit -> ksud -> Manager App
# Signing env: YUKISU_KEYSTORE, YUKISU_KEYSTORE_PASSWORD, YUKISU_KEY_ALIAS, YUKISU_KEY_PASSWORD
# Usage: ./scripts/build.sh [-k KMI] [--yukizygisk|--yukizygisk-off] [--yukizygisk-parts PARTS] [--skip-lkm] [--skip-kasumi] [--kasumi-dir PATH] [-i] [-h]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$REPO_ROOT/out"
KMI="android16-6.12"
ANDROID_ABI="arm64-v8a"
SKIP_LKM=false
SKIP_KASUMI=false
DDK_RELEASE="20260313"
DO_INSTALL=false
ENABLE_YUKIZYGISK=true
YUKIZYGISK_PARTS="all"
# Default Kasumi checkout; override with --kasumi-dir.
KASUMI_DIR="${KASUMI_DIR:-/Volumes/Workspace/Kasumi}"

while [[ $# -gt 0 ]]; do
	case "$1" in
	-k | --kmi)
		KMI="$2"
		shift 2
		;;
	--skip-lkm)
		SKIP_LKM=true
		shift
		;;
	--skip-kasumi)
		SKIP_KASUMI=true
		shift
		;;
	--build-kasumi)
		SKIP_KASUMI=false
		shift
		;;
	--yukizygisk)
		ENABLE_YUKIZYGISK=true
		YUKIZYGISK_PARTS="all"
		shift
		;;
	--yukizygisk-off)
		ENABLE_YUKIZYGISK=false
		shift
		;;
	--yukizygisk-parts)
		ENABLE_YUKIZYGISK=true
		YUKIZYGISK_PARTS="$2"
		shift 2
		;;
	--kasumi-dir)
		KASUMI_DIR="$2"
		shift 2
		;;
	-i | --install)
		DO_INSTALL=true
		shift
		;;
	-h | --help)
		head -5 "$0" | tail -n +2 | sed 's/^# \?//'
		exit 0
		;;
	*)
		echo "Unknown option: $1"
		exit 1
		;;
	esac
done

ANDROID_TARGET=aarch64-linux-android26

detect_ndk_host() {
	if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
		echo "ANDROID_NDK_HOME is required"
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
		echo "Cannot detect NDK prebuilt toolchain"
		exit 1
	fi
}

detect_jobs() {
	if command -v nproc >/dev/null 2>&1; then
		nproc --all
	elif command -v getconf >/dev/null 2>&1; then
		getconf _NPROCESSORS_ONLN
	elif command -v sysctl >/dev/null 2>&1; then
		sysctl -n hw.logicalcpu
	else
		echo 8
	fi
}

NDK_HOST=$(detect_ndk_host)
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$NDK_HOST"
MAKE_JOBS=$(detect_jobs)

echo "=== YukiSU local build ==="
echo "KMI: $KMI | ABI: $ANDROID_ABI | NDK: $ANDROID_NDK_HOME"
echo ""

KSU_YUKIZYGISK_MAKE=""
if [[ "$ENABLE_YUKIZYGISK" == "true" ]]; then
	KSU_YUKIZYGISK_MAKE="CONFIG_KSU_YUKIZYGISK=y"
	if [[ "$YUKIZYGISK_PARTS" == "all" ]]; then
		YUKIZYGISK_PARTS="probe,nl,orch,ctl"
	elif [[ "$YUKIZYGISK_PARTS" == "none" ]]; then
		YUKIZYGISK_PARTS=""
	fi
	IFS=',' read -r -a yz_parts <<<"$YUKIZYGISK_PARTS"
	for part in "${yz_parts[@]}"; do
		case "$part" in
		"" ) ;;
		probe) KSU_YUKIZYGISK_MAKE+=" CONFIG_KSU_YZ_PROBE=y" ;;
		orch) KSU_YUKIZYGISK_MAKE+=" CONFIG_KSU_YZ_ORCH=y" ;;
		nl) KSU_YUKIZYGISK_MAKE+=" CONFIG_KSU_YZ_NL=y" ;;
		ctl) KSU_YUKIZYGISK_MAKE+=" CONFIG_KSU_YZ_CTL=y" ;;
		*)
			echo "Unknown YukiZygisk part: $part"
			exit 1
			;;
		esac
	done
	echo "YukiZygisk kernel hooks: enabled (${YUKIZYGISK_PARTS:-none})"
else
	echo "YukiZygisk kernel hooks: disabled"
fi
echo ""

if [[ "$SKIP_LKM" != "true" ]]; then
	echo ">>> [1/5] Build KernelSU LKM (DDK) ..."
	mkdir -p "$OUT_DIR"
	docker run --rm -v "$REPO_ROOT:/src" -w /src \
		"ghcr.io/ylarod/ddk-min:${KMI}-${DDK_RELEASE}" \
		bash -c "cd kernel && test -f include/uapi/supercall.h && \
	             CONFIG_KSU=m CONFIG_KSU_SUPERKEY=y ${KSU_YUKIZYGISK_MAKE} CC=clang make -j${MAKE_JOBS} && \
	             mkdir -p /src/out && cp kernelsu.ko /src/out/${KMI}_kernelsu.ko && \
	             (llvm-strip -d /src/out/${KMI}_kernelsu.ko 2>/dev/null || true)"
	echo "    LKM: $OUT_DIR/${KMI}_kernelsu.ko"
else
	echo ">>> [1/5] Skip LKM"
fi

echo ">>> [2/5] Build ksuinit ..."
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
	-DCMAKE_ANDROID_ARCH_ABI="$ANDROID_ABI" \
	-DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
	-DCMAKE_C_COMPILER="$CC" \
	-DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_BUILD_TYPE=Release

ninja
echo "    ksuinit built"

arch_suffix="_arm64"

if [[ "$SKIP_KASUMI" != "true" ]]; then
	echo ">>> [2.5/5] Build Kasumi LKM (DDK) from $KASUMI_DIR ..."
	if [[ ! -f "$KASUMI_DIR/src/Makefile" ]]; then
		echo "    missing $KASUMI_DIR/src/Makefile"
		exit 1
	fi
	KASUMI_OUT_DIR="$OUT_DIR/${KMI}-kasumi-lkm"
	mkdir -p "$KASUMI_OUT_DIR"
	# /src is the Kasumi checkout; the DDK image sets KDIR.
	docker run --rm -v "$KASUMI_DIR:/src" -w /src \
		"ghcr.io/ylarod/ddk:${KMI}-${DDK_RELEASE}" \
		bash -c "make -C src ARCH=arm64 -j${MAKE_JOBS} && \
		         (llvm-strip -d src/kasumi_lkm.ko 2>/dev/null || true) && \
		         cp src/kasumi_lkm.ko /src/.kasumi_built.ko"
	# Name expected by lkm.cpp: <KMI>${arch_suffix}_kasumi_lkm.ko
	cp "$KASUMI_DIR/.kasumi_built.ko" "$KASUMI_OUT_DIR/${KMI}${arch_suffix}_kasumi_lkm.ko"
	rm -f "$KASUMI_DIR/.kasumi_built.ko"
	echo "    Kasumi LKM: $KASUMI_OUT_DIR/${KMI}${arch_suffix}_kasumi_lkm.ko"
else
	echo ">>> [2.5/5] Skip Kasumi LKM"
fi

echo ">>> [3/5] Build ksud ..."
KSUD_ASSETS="$REPO_ROOT/userspace/ksud/assets"
mkdir -p "$KSUD_ASSETS"
mkdir -p "$OUT_DIR"

if [[ -f "$OUT_DIR/${KMI}_kernelsu.ko" ]]; then
	cp "$OUT_DIR/${KMI}_kernelsu.ko" "$KSUD_ASSETS/"
else
	echo "    warning: ${KMI}_kernelsu.ko not found"
fi

shopt -s nullglob 2>/dev/null || true
for d in "$OUT_DIR"/*-kasumi-lkm; do
	[[ -d "$d" ]] && cp "$d"/*"${arch_suffix}_kasumi_lkm.ko" "$KSUD_ASSETS/" 2>/dev/null || true
done

cp "$KSUINIT_DIR/build/ksuinit" "$KSUD_ASSETS/"

# Build the standalone magisk-compat su (its own project, like ksuinit) and stage
# it into ksud assets BEFORE ksud configures, so embed_assets picks it up as a
# prebuilt asset -- ksud no longer compiles su itself.
echo ">>> Build su (magisk-compat) ..."
SU_DIR="$REPO_ROOT/userspace/su"
rm -rf "$SU_DIR/build"
mkdir -p "$SU_DIR/build"
cd "$SU_DIR/build"
cmake .. \
	-G Ninja \
	-DCMAKE_SYSTEM_NAME=Android \
	-DCMAKE_ANDROID_ARCH_ABI="$ANDROID_ABI" \
	-DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
	-DCMAKE_C_COMPILER="$CC" \
	-DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_BUILD_TYPE=Release
ninja
cp "$SU_DIR/build/su" "$KSUD_ASSETS/su"
echo "    su staged"

# YukiZygisk payload.
echo ">>> Build YukiZygisk payload ..."
ZCORE_DIR="$REPO_ROOT/userspace/zygisk/core"
rm -rf "$ZCORE_DIR/build"; mkdir -p "$ZCORE_DIR/build"; cd "$ZCORE_DIR/build"
if cmake .. -G Ninja \
	-DCMAKE_SYSTEM_NAME=Android \
	-DCMAKE_ANDROID_ARCH_ABI="$ANDROID_ABI" \
	-DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
	-DCMAKE_C_COMPILER="$CC" \
	-DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_BUILD_TYPE=Release && ninja; then
	cp "$ZCORE_DIR/build/libzygisk.so" "$KSUD_ASSETS/"
	cp "$ZCORE_DIR/build/libyukilinker.so" "$KSUD_ASSETS/"
	cp "$ZCORE_DIR/build/libyukizncore.so" "$KSUD_ASSETS/"
	echo "    staged libzygisk.so + libyukilinker.so + libyukizncore.so"
else
	echo "    YukiZygisk payload build failed; skipped"
fi

KSUD_DIR="$REPO_ROOT/userspace/ksud"
rm -rf "$KSUD_DIR/build"
mkdir -p "$KSUD_DIR/build"
cd "$KSUD_DIR/build"

cmake .. \
	-G Ninja \
	-DCMAKE_SYSTEM_NAME=Android \
	-DCMAKE_ANDROID_ARCH_ABI="$ANDROID_ABI" \
	-DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
	-DCMAKE_C_COMPILER="$CC" \
	-DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_BUILD_TYPE=Release

# su, ksuinit and the .ko assets are all staged into assets/ above (before this
# configure), so ksud just embeds whatever is there -- no in-tree su target.
ninja
echo "    ksud built"

echo ">>> [4/5] Build Manager App ..."
MANAGER_DIR="$REPO_ROOT/manager"
JNILIBS="$MANAGER_DIR/app/src/main/jniLibs/$ANDROID_ABI"
mkdir -p "$JNILIBS"
cp "$KSUD_DIR/build/ksud" "$JNILIBS/libksud.so"

# Signing is passed through env, not gradle.properties.
if [[ -n "${YUKISU_KEYSTORE:-}" && -n "${YUKISU_KEYSTORE_PASSWORD:-}" && -n "${YUKISU_KEY_ALIAS:-}" && -n "${YUKISU_KEY_PASSWORD:-}" ]]; then
	export KEYSTORE_FILE="$YUKISU_KEYSTORE"
	export KEYSTORE_PASSWORD="$YUKISU_KEYSTORE_PASSWORD"
	export KEY_ALIAS="$YUKISU_KEY_ALIAS"
	export KEY_PASSWORD="$YUKISU_KEY_PASSWORD"
	export ORG_GRADLE_PROJECT_KEYSTORE_FILE="$YUKISU_KEYSTORE"
	export ORG_GRADLE_PROJECT_KEYSTORE_PASSWORD="$YUKISU_KEYSTORE_PASSWORD"
	export ORG_GRADLE_PROJECT_KEY_ALIAS="$YUKISU_KEY_ALIAS"
	export ORG_GRADLE_PROJECT_KEY_PASSWORD="$YUKISU_KEY_PASSWORD"
fi

cd "$MANAGER_DIR"
./gradlew assembleRelease --build-cache --no-daemon -PABI="$ANDROID_ABI"
echo "    APK built"

APK_DIR="$MANAGER_DIR/app/build/outputs/apk/release"
echo ""
echo "=== Build complete ==="
echo "APK: $APK_DIR"
ls -la "$APK_DIR"/*.apk 2>/dev/null || true
echo ""

if [[ "$DO_INSTALL" == "true" ]]; then
	apk_files=("$APK_DIR"/*.apk)
	APK_FILE=""
	if [[ ${#apk_files[@]} -gt 0 ]]; then
		APK_FILE="${apk_files[0]}"
	fi
	if [[ -n "$APK_FILE" ]]; then
		echo ">>> Install to device ..."
		adb install -r "$APK_FILE" && echo "APK installed" || echo "APK install failed"
		echo ""
		echo "ksud is embedded in the APK; sync it from the app before reboot."
	fi
else
	echo "Install: adb install -r $APK_DIR/*.apk"
	echo "Or: ./scripts/build.sh --skip-lkm --skip-kasumi -i"
	echo ""
	echo "After installing, sync ksud from the app before reboot."
fi
