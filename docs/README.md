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
> Since version **1.2.0**, the kernel driver uses a **unified codebase** for both LKM and GKI/non-GKI builds.

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/tiann/KernelSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/hymo_chat)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/tiann/KernelSU?logo=gnu)](/LICENSE)

## Features

1. Kernel-based `su` and root access management
2. Module system based on [Magic Mount](https://github.com/5ec1cff/KernelSU)
   > **Note:** YukiSU now delegates all module mounting to the installed *metamodule*; the core no longer handles mount operations.
3. [App Profile](https://kernelsu.org/guide/app-profile.html): Lock up the root power in a cage
4. Tweaks to the manager theme

## Compatibility Status

- YukiSU officially supports Android GKI 2.0 devices (kernel 5.10+).

- Older kernels (4.4+) are also compatible, but the kernel will have to be built manually.

- With more backports, YukiSU can support 3.x kernel (3.4-3.18).

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

- [ShirkNeko](https://afdian.com/a/shirkneko) (maintainer of SukiSU)
- [weishu](https://github.com/sponsors/tiann) (author of KernelSU)

## ShirkNeko's sponsorship list

- [Ktouls](https://github.com/Ktouls) Thanks so much for bringing me support.
- [zaoqi123](https://github.com/zaoqi123) Thanks for the milk tea.
- [wswzgdg](https://github.com/wswzgdg) Many thanks for supporting this project.
- [yspbwx2010](https://github.com/yspbwx2010) Many thanks.
- [DARKWWEE](https://github.com/DARKWWEE) 100 USDT
- [Saksham Singla](https://github.com/TypeFlu) Provide and maintain the website
- [OukaroMF](https://github.com/OukaroMF) Donation of website domain name

## License

- The file in the “kernel” directory is under [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) license.
- Except for the files or directories mentioned above, all other parts are under [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html) license.

## Credit

- [KernelSU](https://github.com/tiann/KernelSU): upstream
- [MKSU](https://github.com/5ec1cff/KernelSU): Magic Mount
- [RKSU](https://github.com/rsuntk/KernelsU): support non-GKI
- [HymoFS](https://github.com/backslashxx/HymoFS): kernel-level filesystem hiding
- [KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch is a key part of the APatch implementation of the kernel module

<details>
<summary>KernelSU's credit</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/): The KernelSU idea.
- [Magisk](https://github.com/topjohnwu/Magisk): The powerful root tool.
- [genuine](https://github.com/brevent/genuine/): APK v2 signature validation.
- [Diamorphine](https://github.com/m0nad/Diamorphine): Some rootkit skills.
</details>
