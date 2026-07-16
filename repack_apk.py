import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


ARCH_TO_TRIPLE = {
    "arm64-v8a": "aarch64-linux-android",
}


def workspace_root() -> Path:
    return Path(__file__).resolve().parent


def normalize_arch_values(values: Iterable[str]) -> List[str]:
    out: List[str] = []
    seen = set()
    for item in values:
        for part in str(item).split(","):
            arch = part.strip()
            if arch and arch not in seen:
                if arch not in ARCH_TO_TRIPLE:
                    supported = ", ".join(ARCH_TO_TRIPLE)
                    raise ValueError(f"Unsupported ABI '{arch}'; supported ABI: {supported}")
                seen.add(arch)
                out.append(arch)
    return out


def version_key(version: str) -> Tuple[int, ...]:
    nums = re.findall(r"\d+", version)
    return tuple(int(n) for n in nums) if nums else (0,)


def find_android_tool(tool_base_name: str) -> Optional[Path]:
    direct = shutil.which(tool_base_name)
    if direct:
        return Path(direct)

    sdk_root = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if not sdk_root:
        return None

    build_tools = Path(sdk_root) / "build-tools"
    if not build_tools.exists():
        return None

    candidates: List[Tuple[Tuple[int, ...], Path]] = []
    names = [tool_base_name]
    if os.name == "nt":
        names = [f"{tool_base_name}.exe", f"{tool_base_name}.bat", tool_base_name]

    for version_dir in build_tools.iterdir():
        if not version_dir.is_dir():
            continue
        for name in names:
            candidate = version_dir / name
            if candidate.exists():
                candidates.append((version_key(version_dir.name), candidate))

    if not candidates:
        return None
    candidates.sort(key=lambda item: item[0], reverse=True)
    return candidates[0][1]


def find_strip_tool() -> Optional[Path]:
    ndk_root = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK")
    if not ndk_root:
        sdk_root = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
        ndk_dir = Path(sdk_root or "") / "ndk"
        if ndk_dir.exists():
            versions = sorted(
                [d for d in ndk_dir.iterdir() if d.is_dir()],
                key=lambda d: version_key(d.name),
                reverse=True,
            )
            if versions:
                ndk_root = str(versions[0])

    if ndk_root:
        prebuilt_root = Path(ndk_root) / "toolchains" / "llvm" / "prebuilt"
        if prebuilt_root.exists():
            for prebuilt in prebuilt_root.iterdir():
                bin_dir = prebuilt / "bin"
                for name in ("llvm-strip", "llvm-strip.exe", "strip", "strip.exe"):
                    candidate = bin_dir / name
                    if candidate.exists():
                        return candidate

    for name in ("llvm-strip", "strip"):
        found = shutil.which(name)
        if found:
            return Path(found)
    return None


def run_cmd(args: List[str], fail_msg: str) -> None:
    proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        output = proc.stdout.strip()
        raise RuntimeError(f"{fail_msg}\nCommand: {' '.join(args)}\n{output}")


def find_latest_apk(app_build_type: str) -> Path:
    apk_dir = workspace_root() / "manager" / "app" / "build" / "outputs" / "apk" / app_build_type
    apks = sorted(apk_dir.glob("*.apk"), key=lambda path: path.stat().st_mtime)
    if not apks:
        raise FileNotFoundError(f"No APK found under: {apk_dir}")
    if len(apks) > 1:
        print(f"[WARN] Multiple APKs found, using latest: {apks[-1]}", file=sys.stderr)
    return apks[-1]


def find_ksud_binaries_by_arch(ksud_build_type: str, arches: List[str]) -> Dict[str, Path]:
    result: Dict[str, Path] = {}
    for arch in arches:
        triple = ARCH_TO_TRIPLE.get(arch)
        if not triple:
            print(f"[WARN] Unknown arch '{arch}', cannot map to target triple.", file=sys.stderr)
            continue
        candidate = workspace_root() / "target" / triple / ksud_build_type / "ksud"
        if candidate.exists():
            result[arch] = candidate
        else:
            print(f"[WARN] ksud not found for {arch}: {candidate}", file=sys.stderr)
    return result


def collect_existing_arches(apk_path: Path) -> List[str]:
    arches: List[str] = []
    seen = set()
    with ZipFile(apk_path, "r") as zf:
        for name in zf.namelist():
            if not name.startswith("lib/"):
                continue
            parts = name.split("/")
            if len(parts) >= 3 and parts[1] not in seen:
                seen.add(parts[1])
                arches.append(parts[1])
    return arches


def collect_existing_ksud_arches(apk_path: Path) -> List[str]:
    arches: List[str] = []
    seen = set()
    with ZipFile(apk_path, "r") as zf:
        for name in zf.namelist():
            if not (name.startswith("lib/") and name.endswith("/libksud.so")):
                continue
            parts = name.split("/")
            if len(parts) >= 3 and parts[1] not in seen:
                seen.add(parts[1])
                arches.append(parts[1])
    return arches


def strip_binary(src: Path, strip_tool: Path, tmp_dir: Path) -> bytes:
    dst = tmp_dir / src.name
    shutil.copy2(src, dst)
    run_cmd([str(strip_tool), "--strip-all", str(dst)], "strip failed")
    return dst.read_bytes()


def repack_apk(
    apk_path: Path,
    out_unsigned_path: Path,
    arches: List[str],
    ksud_by_arch: Dict[str, Path],
    strip_tool: Optional[Path],
) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        ksud_bytes_by_arch: Dict[str, bytes] = {}
        for arch, ksud_path in ksud_by_arch.items():
            if strip_tool is not None:
                ksud_bytes_by_arch[arch] = strip_binary(ksud_path, strip_tool, tmp_dir)
            else:
                ksud_bytes_by_arch[arch] = ksud_path.read_bytes()

        with ZipFile(apk_path, "r") as zin, ZipFile(out_unsigned_path, "w") as zout:
            for info in zin.infolist():
                name = info.filename

                if name.startswith("lib/") and arches:
                    parts = name.split("/")
                    if len(parts) >= 3 and parts[1] not in arches:
                        continue

                if name.startswith("lib/") and name.endswith("/libksud.so"):
                    parts = name.split("/")
                    if len(parts) >= 3 and parts[1] in ksud_bytes_by_arch:
                        continue

                new_info = ZipInfo(filename=name, date_time=info.date_time)
                new_info.compress_type = info.compress_type
                new_info.external_attr = info.external_attr
                new_info.comment = info.comment
                new_info.create_system = info.create_system
                new_info.extra = info.extra
                data = zin.read(name)
                if info.compress_type == ZIP_DEFLATED:
                    zout.writestr(new_info, data, compress_type=ZIP_DEFLATED)
                else:
                    zout.writestr(new_info, data)

            for arch in arches:
                ksud_bytes = ksud_bytes_by_arch.get(arch)
                if ksud_bytes is None:
                    continue
                entry = ZipInfo(filename=f"lib/{arch}/libksud.so")
                entry.compress_type = ZIP_DEFLATED
                zout.writestr(entry, ksud_bytes)


def assert_required_libs(apk_path: Path, arches: List[str]) -> None:
    with ZipFile(apk_path, "r") as zf:
        names = set(zf.namelist())
    missing = [arch for arch in arches if f"lib/{arch}/libksud.so" not in names]
    if missing:
        raise RuntimeError("Missing libksud.so in APK for architecture(s): " + ", ".join(missing))


def validate_signing_args(args: argparse.Namespace) -> None:
    missing = [
        name
        for name, value in (
            ("keystore path", args.keystore_path),
            ("key alias", args.key_alias),
            ("keystore password", args.keystore_pass),
            ("key password", args.key_pass),
        )
        if not str(value or "").strip()
    ]
    if missing:
        raise RuntimeError("Signing config is incomplete, missing: " + ", ".join(missing))
    if not Path(args.keystore_path).exists():
        raise FileNotFoundError(f"Keystore not found: {args.keystore_path}")


def do_repack(args: argparse.Namespace) -> int:
    app_build_type = args.app_build_type or "release"
    ksud_build_type = args.ksud_build_type or "release"
    arches = normalize_arch_values(args.arch or [])

    apk = find_latest_apk(app_build_type)
    if not arches:
        arches = collect_existing_arches(apk) or ["arm64-v8a"]
        print(f"[INFO] No arch configured, using: {', '.join(arches)}")

    unsupported_arches = [arch for arch in arches if arch not in ARCH_TO_TRIPLE]
    if unsupported_arches:
        supported = ", ".join(ARCH_TO_TRIPLE)
        raise RuntimeError(
            "APK contains unsupported ABI(s): "
            + ", ".join(unsupported_arches)
            + f"; supported ABI: {supported}"
        )

    ksud_by_arch = find_ksud_binaries_by_arch(ksud_build_type, arches)
    missing_ksud = [arch for arch in arches if arch not in ksud_by_arch]
    if missing_ksud:
        existing_ksud = set(collect_existing_ksud_arches(apk))
        missing_in_apk = [arch for arch in missing_ksud if arch not in existing_ksud]
        if missing_in_apk:
            raise RuntimeError(
                "ksud binary not found and APK has no existing libksud.so for architecture(s): "
                + ", ".join(missing_in_apk)
            )
        print(
            "[WARN] ksud binary not found for architecture(s): "
            + ", ".join(missing_ksud)
            + ". Using existing libksud.so from input APK.",
            file=sys.stderr,
        )

    out_dir = Path(args.out_dir).resolve() if args.out_dir else workspace_root() / "dist"
    out_dir.mkdir(parents=True, exist_ok=True)
    output_name = args.output_name or apk.stem
    unsigned_apk = out_dir / f"{output_name}-repack-unsigned.apk"
    aligned_apk = out_dir / f"{output_name}-repack-aligned.apk"
    signed_apk = out_dir / f"{output_name}.apk"

    for stale in (unsigned_apk, aligned_apk, signed_apk):
        if stale.exists():
            stale.unlink()

    strip_tool: Optional[Path] = None
    if args.strip:
        strip_tool = find_strip_tool()
        if strip_tool is None:
            print("[WARN] strip requested but no strip tool found; skipping strip.", file=sys.stderr)
        else:
            print(f"[INFO] Strip tool: {strip_tool}")

    try:
        repack_apk(apk, unsigned_apk, arches, ksud_by_arch, strip_tool)
        assert_required_libs(unsigned_apk, arches)

        zipalign = find_android_tool("zipalign")
        if zipalign is None:
            raise FileNotFoundError("zipalign not found in PATH or Android SDK build-tools")
        run_cmd([str(zipalign), "-P", "16", "-f", "4", str(unsigned_apk), str(aligned_apk)], "zipalign failed")

        validate_signing_args(args)
        apksigner = find_android_tool("apksigner")
        if apksigner is None:
            raise FileNotFoundError("apksigner not found in PATH or Android SDK build-tools")
        run_cmd(
            [
                str(apksigner),
                "sign",
                "--v1-signing-enabled",
                "false",
                "--v2-signing-enabled",
                "true",
                "--v3-signing-enabled",
                "false",
                "--v4-signing-enabled",
                "false",
                "--ks",
                str(Path(args.keystore_path).resolve()),
                "--ks-key-alias",
                args.key_alias,
                "--ks-pass",
                f"pass:{args.keystore_pass}",
                "--key-pass",
                f"pass:{args.key_pass}",
                "--out",
                str(signed_apk),
                str(aligned_apk),
            ],
            "apksigner failed",
        )
    finally:
        for tmp in (unsigned_apk, aligned_apk):
            if tmp.exists():
                tmp.unlink()

    ksud_desc = ", ".join(f"{arch}={path}" for arch, path in ksud_by_arch.items()) or "NOT FOUND"
    print(f"Input APK : {apk}")
    print(f"ksud      : {ksud_desc}")
    print(f"Arch      : {', '.join(arches)}")
    print(f"Output    : {signed_apk}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Repack manager APK with current ksud binaries.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    repack = subparsers.add_parser("repack", help="Repack and resign APK")
    repack.add_argument("-b", "--app-build-type", help="APK build type, e.g. release")
    repack.add_argument("-t", "--ksud-build-type", help="ksud build type, e.g. release")
    repack.add_argument("-a", "--arch", action="append", help="Target ABI, repeat or use comma list")
    repack.add_argument("-K", "--keystore-path", help="Keystore path")
    repack.add_argument("-A", "--key-alias", help="Key alias")
    repack.add_argument("-P", "--keystore-pass", help="Keystore password")
    repack.add_argument("-S", "--key-pass", help="Private key password")
    repack.add_argument("-n", "--output-name", help="Base name for output APK")
    repack.add_argument("-o", "--out-dir", help="Output directory")
    repack.add_argument("--strip", action="store_true", help="Strip libksud.so before packing")
    repack.set_defaults(func=do_repack)

    return parser


def main() -> int:
    os.chdir(workspace_root())
    parser = build_parser()
    args = parser.parse_args()
    try:
        return args.func(args)
    except Exception as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
