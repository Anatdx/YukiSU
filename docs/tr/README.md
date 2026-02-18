# YukiSU

<img align='right' src='../YukiSU-mini.svg' width='220px' alt="yukisu logo">


[English](../README.md) | [简体中文](../zh/README.md) | [日本語](../ja/README.md) | **Türkçe** | [Русский](../ru/README.md)

Android cihazlar için çekirdek tabanlı root çözümü; [`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra) projesinden fork edilmiş, gereksiz kısımlar çıkarılmış ve bazı değişiklikler eklenmiştir.

> **⚠️ Önemli Duyuru**
>
> YukiSU **tamamen C++ ile yeniden yazılmıştır** (önceden Rust tabanlıydı). Bu nedenle YukiSU, diğer KernelSU fork’larından farklı davranabilir. Sorun yaşarsanız lütfen üst akış projelerine değil bize bildirin.
>
> Eski Rust sürümü [`classic`](https://github.com/Anatdx/YukiSU/tree/classic) dalında durmaktadır.
>

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/tiann/KernelSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/hymo_chat)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/tiann/KernelSU?logo=gnu)](/LICENSE)

## Özellikler

1. Çekirdek tabanlı `su` ve root erişim yönetimi
2. [Magic Mount](https://github.com/5ec1cff/KernelSU) tabanlı modül sistemi
   > **Not:** YukiSU artık tüm modül bağlama işlemlerini yüklü *metamodule*’e bırakmaktadır; çekirdek bağlama işlemlerini yapmaz.
3. [App Profile](https://kernelsu.org/guide/app-profile.html): Root yetkisini kafese alın
4. Yönetici teması ayarları

## Uyumluluk

- YukiSU, Android GKI 2.0 cihazlarını (çekirdek 5.10+) resmen destekler.

- Eski çekirdekler (4.4+) de uyumludur; ancak çekirdeği sizin derlemeniz gerekir.

- Daha fazla geri taşıma ile 3.x çekirdek (3.4–3.18) desteklenebilir.

- Şu an yalnızca `arm64-v8a`, `armeabi-v7a (bare)` ve `X86_64` (bazıları) desteklenmektedir.

## Kurulum

[`guide/installation.md`](guide/installation.md) konusuna bakın.

## Entegrasyon

[`guide/how-to-integrate.md`](guide/how-to-integrate.md) konusuna bakın.

## Çeviri

Yönetici için çeviri göndermek istiyorsanız [Crowdin](https://crowdin.com/project/YukiSU) adresine gidin.

## Sorun Giderme

1. Yönetici uygulaması kaldırılınca cihaz takılıyor mu?
   _com.sony.playmemories.mobile_ uygulamasını kaldırın.

## Sponsorlar

- [ShirkNeko](https://afdian.com/a/shirkneko) (SukiSU bakımcısı)
- [weishu](https://github.com/sponsors/tiann) (KernelSU yazarı)

## Lisans

- “kernel” dizinindeki dosyalar [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) lisanslıdır.
- Bunlar dışındaki tüm kısımlar [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html) lisanslıdır.

## Katkıda Bulunanlar

- [KernelSU](https://github.com/tiann/KernelSU): Üst akış
- [MKSU](https://github.com/5ec1cff/KernelSU): Magic Mount
- [RKSU](https://github.com/rsuntk/KernelsU): non-GKI desteği
- [KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch, APatch çekirdek modülü uygulamasının önemli bir parçasıdır

<details>
<summary>KernelSU katkıda bulunanları</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/): KernelSU fikri
- [Magisk](https://github.com/topjohnwu/Magisk): Güçlü root aracı
- [genuine](https://github.com/brevent/genuine/): APK v2 imza doğrulama
- [Diamorphine](https://github.com/m0nad/Diamorphine): Rootkit becerileri
</details>
