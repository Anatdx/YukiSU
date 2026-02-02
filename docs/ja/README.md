# YukiSU

<img align='right' src='YukiSU-mini.svg' width='220px' alt="yukisu logo">


[English](../README.md) | [简体中文](../zh/README.md) | **日本語** | [Türkçe](../tr/README.md) | [Русский](../ru/README.md)

[`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra) をフォークした Android デバイスのカーネルベース root ソリューション。不要なものを削除し、興味深い変更を加えました。

> **⚠️ 重要なお知らせ**
>
> YukiSU のユーザースペースプログラムは **C++ で完全に書き直されました**（以前は Rust ベース）。これにより、YukiSU の動作は他の KernelSU フォークと異なる場合があります。問題が発生した場合は、上流プロジェクトではなく、私たちに報告してください。
>
> クラシック Rust バージョンは [`classic`](https://github.com/Anatdx/YukiSU/tree/classic) ブランチに保存されています。
>
> バージョン **1.2.0** 以降、カーネルドライバーは LKM と GKI/non-GKI ビルドの両方に対応した**統一コードベース**を使用しています。

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/tiann/KernelSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/hymo_chat)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/tiann/KernelSU?logo=gnu)](/LICENSE)

## 特徴

1. カーネルベースの `su` と root アクセス管理
2. [Magic Mount](https://github.com/5ec1cff/KernelSU) に基づくモジュールシステム
   > **Note:** YukiSU はすべてのモジュールマウントをインストールされた *metamodule* に委任しています。コア自体はマウント操作を処理しなくなりました。
3. [App Profile](https://kernelsu.org/ja_JP/guide/app-profile.html): root 権限をケージに閉じ込める
4. non-GKI と GKI 1.0 をサポート
5. [HymoFS](https://github.com/backslashxx/HymoFS) を内蔵し、カーネルレベルでのファイルシステム隠蔽を実現（susfs の代替）
6. マネージャーテーマの調整

> **注意:** YukiSU は susfs をサポートしなくなりました。HymoFS がファイルシステム隠蔽のための内蔵ソリューションです。

## 互換性状態

- YukiSU は Android GKI 2.0 デバイス（カーネル 5.10+）を公式にサポートしています。

- 古いカーネル（4.4+）も互換性がありますが、カーネルを手動でビルドする必要があります。

- 追加のバックポートにより、YukiSU は 3.x カーネル（3.4-3.18）をサポートできます。

- 現在、`arm64-v8a`、`armeabi-v7a (bare)`、および `X86_64`（一部）のみサポートしています。

## インストール

[`guide/installation.md`](guide/installation.md) を参照してください

## 統合

[`guide/how-to-integrate.md`](guide/how-to-integrate.md) を参照してください

## 翻訳

マネージャーの翻訳を提出する場合は、[Crowdin](https://crowdin.com/project/YukiSU) にアクセスしてください。

## トラブルシューティング

1. マネージャーアプリのアンインストール時にデバイスがフリーズする？
   _com.sony.playmemories.mobile_ をアンインストールしてください

## スポンサー

- [ShirkNeko](https://afdian.com/a/shirkneko) (SukiSU メンテナー)
- [weishu](https://github.com/sponsors/tiann) (KernelSU 作者)

## ライセンス

- 「kernel」ディレクトリ内のファイルは [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) ライセンスです。
- 上記以外のすべての部分は [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html) ライセンスです。

## クレジット

- [KernelSU](https://github.com/tiann/KernelSU): 上流
- [MKSU](https://github.com/5ec1cff/KernelSU): Magic Mount
- [RKSU](https://github.com/rsuntk/KernelsU): non-GKI サポート
- [HymoFS](https://github.com/backslashxx/HymoFS): カーネルレベルファイルシステム隠蔽
- [KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch は APatch カーネルモジュール実装の重要な部分です

<details>
<summary>KernelSU のクレジット</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/): KernelSU のアイデア
- [Magisk](https://github.com/topjohnwu/Magisk): 強力な root ツール
- [genuine](https://github.com/brevent/genuine/): APK v2 署名検証
- [Diamorphine](https://github.com/m0nad/Diamorphine): rootkit スキル
</details>
- [KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch はカーネルモジュールの APatch 実装の重要な部分での活用
