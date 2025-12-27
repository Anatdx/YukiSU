# YukiSU
<img align='right' src='YukiSU-mini.svg' width='220px' alt="sukisu logo">


[English](../README.md) | **简体中文** | [日本語](../ja/README.md) | [Türkçe](../tr/README.md) | [Русский](../ru/README.md)

一个 Android 上基于内核的 root 方案，由 [`/SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra) 分叉而来，去掉了一些没用的东西，增加了一些有趣的变更。

> **⚠️ 重要提示**
>
> YukiSU 的用户空间程序已**完全用 C++ 重写**（原先基于 Rust）。这意味着 YukiSU 的行为可能与其他 KernelSU 分支有所不同。如果您遇到任何问题，请向我们反馈，而不是向上游项目反馈。
>
> 经典的 Rust 版本保留在 [`classic`](https://github.com/Anatdx/YukiSU/tree/classic) 分支中。

[![最新发行](https://img.shields.io/github/v/release/YukiSU/YukiSU?label=Release&logo=github)](https://github.com/tiann/KernelSU/releases/latest)
[![频道](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/hymo_chat)
[![协议: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub 协议](https://img.shields.io/github/license/tiann/KernelSU?logo=gnu)](/LICENSE)

## 特性

1. 基于内核的 `su` 和权限管理。
2. 基于 [Magic Mount](https://github.com/5ec1cff/KernelSU) 的模块系统。
   > **Note:** YukiSU now delegates all module mounting to the installed *metamodule*; the core no longer handles mount operations.
3. [App Profile](https://kernelsu.org/zh_CN/guide/app-profile.html): 把 Root 权限关进笼子里。
4. 支持 non-GKI 与 GKI 1.0。
5. KPM 支持
6. 可调整管理器外观，可自定义 susfs 配置。

## 兼容状态

- KernelSU 官方支持 GKI 2.0 的设备（内核版本 5.10 以上）。

- 旧内核也是兼容的（最低 4.14+），不过需要自己编译内核。

- 通过更多的反向移植，KernelSU 可以支持 3.x 内核（3.4-3.18）。

- 目前支持架构 : `arm64-v8a`、`armeabi-v7a (bare)`、`X86_64`。

## 安装指导

查看 [`guide/installation.md`](guide/installation.md)

## 集成指导

查看 [`guide/how-to-integrate.md`](guide/how-to-integrate.md)

## 参与翻译

要将 YukiSU 翻译成您的语言，或完善现有的翻译，请使用 [Crowdin](https://crowdin.com/project/YukiSU).

## KPM 支持

- 基于 KernelPatch 开发，移除了与 KernelSU 重复的功能。
- 正在进行（WIP）：通过集成附加功能来扩展 APatch 兼容性，以确保跨不同实现的兼容性。

**开源仓库**: [https://github.com/ShirkNeko/YukiSU_KernelPatch_patch](https://github.com/ShirkNeko/YukiSU_KernelPatch_patch)

**KPM 模板**: [https://github.com/udochina/KPM-Build-Anywhere](https://github.com/udochina/KPM-Build-Anywhere)

> [!Note]
>
> 1. 需要 `CONFIG_KPM=y`
> 2. Non-GKI 设备需要 `CONFIG_KALLSYMS=y` and `CONFIG_KALLSYMS_ALL=y`
> 3. 对于低于 `4.19` 的内核，需要从 `4.19` 的 `set_memory.h` 进行反向移植。

## 故障排除

1. 卸载管理器后系统卡住？
   卸载 _com.sony.playmemories.mobile_

## 许可证

- 目录 `kernel` 下所有文件为 [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)。
- 除上述文件及目录的其他部分均为 [GPL-3.0-or-later](https://www.gnu.org/licenses/gpl-3.0.html)。

## 赞助

- [ShirkNeko](https://afdian.com/a/shirkneko) (SukiSU 主要维护者)
- [weishu](https://github.com/sponsors/tiann) (KernelSU 作者)

## 鸣谢

- [KernelSU](https://github.com/tiann/KernelSU): 上游
- [RKSU](https://github.com/rsuntk/KernelsU): non-GKI 支持
- [susfs](https://gitlab.com/simonpunk/susfs4ksu): 隐藏内核补丁以及用户空间模组的 KernelSU 附件
- [KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch 是内核模块 APatch 实现的关键部分

<details>
<summary>KernelSU 的鸣谢</summary>

- [kernel-assisted-superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/)：KernelSU 的灵感。
- [Magisk](https://github.com/topjohnwu/Magisk)：强大的 root 工具箱。
- [genuine](https://github.com/brevent/genuine/)：apk v2 签名验证。
- [Diamorphine](https://github.com/m0nad/Diamorphine)：一些 rootkit 技巧。
</details>
