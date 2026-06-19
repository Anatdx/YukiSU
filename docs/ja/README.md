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

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/Anatdx/YukiSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/manosaba)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/Anatdx/YukiSU?logo=gnu)](/LICENSE)

## 特徴

1. カーネルベースの `su` と root アクセス管理
2. 旧 HymoFS パスを置き換える、組み込み Kasumi ハイブリッドマウントモジュールバックエンド
   > **Note:** YukiSU は SUSFS をサポートしなくなりました。外部カーネルパッケージ名に SUSFS が残る場合でも、YukiSU 本体は Kasumi を使用します。
3. [App Profile](https://kernelsu.org/ja_JP/guide/app-profile.html) とアプリごとの non-root プロファイル制御
4. 組み込みパッケージ署名以外の信頼済みマネージャーに対応する Dynamic Manager
5. APatch 風の SuperKey 認証。コンパイル時キー、または `ksud` による LKM へのキー注入に対応
6. ADB root、sulog、SELinux hide、モジュール `init.rc` 注入、現在の KernelSU userspace 挙動を C++ `ksud` スタックへ同期
7. arm64 と x86_64 LKM をサポートする TSR ベースの sucompat/syscall hook 基盤
8. Kasumi 設定、SuperUser スワイプ操作、ログビューア、ソフト再起動、WebUI 修正を含むマネージャー更新

## 互換性

- YukiSU は現在、ロード可能カーネルモジュールパス（`CONFIG_KSU=m`）のみをサポートします。組み込みの `CONFIG_KSU=y` はサポートされません。

- YukiSU は Android GKI 2.0 デバイス（カーネル 5.10+）の LKM モードを公式にサポートしています。古いカーネルや non-GKI カーネルでは、デバイス固有のソース統合が必要になる場合があります。

- 現在サポートしているのは `arm64-v8a`、`armeabi-v7a (bare)`、`X86_64`（一部）のみです。

## インストール

[`guide/installation.md`](guide/installation.md) を参照してください。

## 統合

[`guide/how-to-integrate.md`](guide/how-to-integrate.md) を参照してください。

## トラブルシューティング

1. マネージャーアプリをアンインストールしたらデバイスが固まる？
   _com.sony.playmemories.mobile_ をアンインストールしてください。

## スポンサー

- [Anatdx](https://afd.anatdx.moe)（YukiSU メンテナー）
- [ShirkNeko](https://afdian.com/a/shirkneko)（SukiSU メンテナー）
- [weishu](https://github.com/sponsors/tiann)（KernelSU 作者）

## ライセンス

- 「kernel」ディレクトリ内のファイルは [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) です。
- 上記以外は [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html) です。

## クレジット

- [KernelSU](https://github.com/tiann/KernelSU): 上流
- Kasumi: 組み込みハイブリッドマウントバックエンド
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
