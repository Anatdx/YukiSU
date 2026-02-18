# YukiSU

<img align='right' src='../YukiSU-mini.svg' width='220px' alt="yukisu logo">


[English](../README.md) | [简体中文](../zh/README.md) | **日本語** | [Türkçe](../tr/README.md) | [Русский](../ru/README.md)

Android デバイス向けのカーネルベース root ソリューション。[`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra) からフォークし、不要な部分を削除し、いくつかの変更を加えています。

> **⚠️ 重要なお知らせ**
>
> YukiSU は**完全に C++ で書き直されました**（以前は Rust ベース）。このため、YukiSU の挙動は他の KernelSU フォークと異なる場合があります。問題があれば、上游プロジェクトではなく当方へ報告してください。
>
> 従来の Rust 版は [`classic`](https://github.com/Anatdx/YukiSU/tree/classic) ブランチに残しています。
>

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/tiann/KernelSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/hymo_chat)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/tiann/KernelSU?logo=gnu)](/LICENSE)

## 特徴

1. カーネルベースの `su` と root アクセス管理
2. [Magic Mount](https://github.com/5ec1cff/KernelSU) ベースのモジュールシステム
   > **Note:** YukiSU はモジュールのマウントをすべてインストール済みの *metamodule* に委譲しています。コアではマウント処理を行いません。
3. [App Profile](https://kernelsu.org/ja_JP/guide/app-profile.html): root 権限をケージで管理
4. マネージャーのテーマ調整

## 互換性

- YukiSU は Android GKI 2.0 デバイス（カーネル 5.10+）を公式にサポートしています。

- 旧カーネル（4.4+）も互換がありますが、カーネルは手動ビルドが必要です。

- バックポートを増やすことで 3.x カーネル（3.4–3.18）にも対応可能です。

- 現在サポートしているのは `arm64-v8a`、`armeabi-v7a (bare)`、`X86_64`（一部）のみです。

## インストール

[`guide/installation.md`](guide/installation.md) を参照してください。

## 統合

[`guide/how-to-integrate.md`](guide/how-to-integrate.md) を参照してください。

## 翻訳

マネージャーの翻訳を投稿する場合は [Crowdin](https://crowdin.com/project/YukiSU) をご利用ください。

## トラブルシューティング

1. マネージャーアプリをアンインストールしたらデバイスが固まる？
   _com.sony.playmemories.mobile_ をアンインストールしてください。

## スポンサー

- [ShirkNeko](https://afdian.com/a/shirkneko)（SukiSU メンテナー）
- [weishu](https://github.com/sponsors/tiann)（KernelSU 作者）

## ライセンス

- 「kernel」ディレクトリ内のファイルは [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) です。
- 上記以外は [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html) です。

## クレジット

- [KernelSU](https://github.com/tiann/KernelSU): 上流
- [MKSU](https://github.com/5ec1cff/KernelSU): Magic Mount
- [RKSU](https://github.com/rsuntk/KernelsU): non-GKI サポート
- [KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch は APatch カーネルモジュール実装の重要な一部です

<details>
<summary>KernelSU のクレジット</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/): KernelSU のアイデア
- [Magisk](https://github.com/topjohnwu/Magisk): 強力な root ツール
- [genuine](https://github.com/brevent/genuine/): APK v2 署名検証
- [Diamorphine](https://github.com/m0nad/Diamorphine): rootkit 関連の技術
</details>
