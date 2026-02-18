# YukiSU
<img align='right' src='../YukiSU-mini.svg' width='220px' alt="yukisu logo">


[English](../README.md) | **简体中文** | [日本語](../ja/README.md) | [Türkçe](../tr/README.md) | [Русский](../ru/README.md)

一个基于内核的 Android root 方案，从 [`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra) 分叉而来，去掉了一些无用的部分，并增加了一些有趣的变更。

> **⚠️ 重要提示**
>
> YukiSU 已**完全用 C++ 重写**（原先基于 Rust）。此次重写意味着 YukiSU 的行为可能与其他 KernelSU 分支有所不同。若遇到问题，请向我们反馈，而非上游项目。
>
> 经典的 Rust 版本保留在 [`classic`](https://github.com/Anatdx/YukiSU/tree/classic) 分支中。
>

[![最新发行](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/tiann/KernelSU/releases/latest)
[![频道](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/hymo_chat)
[![协议: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub 协议](https://img.shields.io/github/license/tiann/KernelSU?logo=gnu)](/LICENSE)

## 特性

1. 基于内核的 `su` 与 root 权限管理
2. 基于 [Magic Mount](https://github.com/5ec1cff/KernelSU) 的模块系统
   > **说明：** YukiSU 现已将全部模块挂载交由已安装的 *metamodule* 处理；核心不再负责挂载操作。
3. [App Profile](https://kernelsu.org/zh_CN/guide/app-profile.html)：将 root 权限关进笼子
4. 管理器主题微调

## 兼容状态

- YukiSU 官方支持 Android GKI 2.0 设备（内核 5.10+）。

- 旧内核（4.4+）也兼容，但需自行编译内核。

- 通过更多反向移植，可支持 3.x 内核（3.4–3.18）。

- 目前仅支持 `arm64-v8a`、`armeabi-v7a (bare)` 与 `X86_64`（部分）。

## 安装

参见 [`guide/installation.md`](guide/installation.md)

## 集成

参见 [`guide/how-to-integrate.md`](guide/how-to-integrate.md)

## 翻译

如需提交管理器翻译，请前往 [Crowdin](https://crowdin.com/project/YukiSU)。

## 故障排除

1. 卸载管理器后设备卡住？
   请卸载 _com.sony.playmemories.mobile_

## 赞助

- [ShirkNeko](https://afdian.com/a/shirkneko)（SukiSU 维护者）
- [weishu](https://github.com/sponsors/tiann)（KernelSU 作者）

## 许可证

- “kernel” 目录下文件采用 [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)。
- 上述以外的其余部分采用 [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html)。

## 鸣谢

- [KernelSU](https://github.com/tiann/KernelSU)：上游
- [MKSU](https://github.com/5ec1cff/KernelSU)：Magic Mount
- [RKSU](https://github.com/rsuntk/KernelsU)：non-GKI 支持
- [KernelPatch](https://github.com/bmax121/KernelPatch)：KernelPatch 为 APatch 内核模块实现的关键部分

<details>
<summary>KernelSU 鸣谢</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/)：KernelSU 创意来源
- [Magisk](https://github.com/topjohnwu/Magisk)：强大 root 工具
- [genuine](https://github.com/brevent/genuine/)：APK v2 签名校验
- [Diamorphine](https://github.com/m0nad/Diamorphine)：部分 rootkit 技巧
</details>
