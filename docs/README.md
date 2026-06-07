# YukiSU

<img align='right' src='YukiSU-mini.svg' width='220px' alt="yukisu logo">


**English** | [简体中文](./zh/README.md) | [日本語](./ja/README.md) | [Türkçe](./tr/README.md) | [Русский](./ru/README.md)

A kernel-based root solution for Android devices, forked from [`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra), removed some useless stuff, and added some interesting changes.

> **⚠️ Important Notice**
>
> YukiSU has been **completely rewritten in C++** (previously Rust-based). This rewrite means that YukiSU may behave differently from other KernelSU forks. If you encounter any issues, please report them to us rather than to upstream projects.
>
> The classic Rust version is preserved in the [`classic`](https://github.com/Anatdx/YukiSU/tree/classic) branch.
>

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/Anatdx/YukiSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/manosaba)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/Anatdx/YukiSU?logo=gnu)](/LICENSE)

## Features

1. Kernel-based `su` and root access management
2. Built-in Kasumi hybrid mounting module backend, replacing the old HymoFS path
   > **Note:** YukiSU no longer supports SUSFS. External kernel packages may still mention SUSFS, but YukiSU uses Kasumi.
3. [App Profile](https://kernelsu.org/guide/app-profile.html) and per-app non-root profile controls
4. Dynamic manager support for trusted manager apps beyond the built-in package signature path
5. APatch-style SuperKey authentication, with compile-time keys or keys patched into the LKM by `ksud`
6. ADB root, sulog, SELinux hide, module `init.rc` injection, and current KernelSU userspace behavior synced into the C++ `ksud` stack
7. TSR-based sucompat/syscall hook infrastructure with arm64 and x86_64 LKM support
8. Manager updates including Kasumi configuration, SuperUser swipe actions, log viewing, soft reboot, and WebUI fixes

## Compatibility Status

- YukiSU currently supports only the loadable kernel module path (`CONFIG_KSU=m`). Built-in `CONFIG_KSU=y` is no longer supported.

- YukiSU officially supports Android GKI 2.0 devices (kernel 5.10+) in LKM mode. Legacy and non-GKI kernels may require device-specific source integration.

- Currently, only `arm64-v8a`, `armeabi-v7a (bare)` and `X86_64`(some) are supported.

## Installation

See [`guide/installation.md`](guide/installation.md)

## Integration

See [`guide/how-to-integrate.md`](guide/how-to-integrate.md)

## Translation

If you need to submit a translation for the manager, please go to [Crowdin](https://crowdin.com/project/YukiSU).

## Troubleshooting

1. Device stuck upon manager app uninstallation?
   Uninstall _com.sony.playmemories.mobile_

## Sponsor

- [Anatdx](https://afd.anatdx.com) (maintainer of YukiSU)
- [ShirkNeko](https://afdian.com/a/shirkneko) (maintainer of SukiSU)
- [weishu](https://github.com/sponsors/tiann) (author of KernelSU)

## License

- The file in the “kernel” directory is under [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) license.
- Except for the files or directories mentioned above, all other parts are under [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html) license.

## Credit

- [KernelSU](https://github.com/tiann/KernelSU): upstream
- Kasumi: built-in hybrid mounting backend
- [MKSU](https://github.com/5ec1cff/KernelSU): Magic Mount
- [RKSU](https://github.com/rsuntk/KernelsU): support non-GKI
- [KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch is a key part of the APatch implementation of the kernel module

<details>
<summary>KernelSU's credit</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/): The KernelSU idea.
- [Magisk](https://github.com/topjohnwu/Magisk): The powerful root tool.
- [genuine](https://github.com/brevent/genuine/): APK v2 signature validation.
- [Diamorphine](https://github.com/m0nad/Diamorphine): Some rootkit skills.
</details>
