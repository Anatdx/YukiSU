# YukiSU

<img align='right' src='../YukiSU-mini.svg' width='220px' alt="yukisu logo">


[English](../README.md) | [简体中文](../zh/README.md) | [日本語](../ja/README.md) | **Türkçe** | [Русский](../ru/README.md)

Android cihazlar için çekirdek tabanlı root çözümü; [`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra) projesinden fork edilmiş, gereksiz kısımlar çıkarılmış ve bazı değişiklikler eklenmiştir.

> **⚠️ Önemli Duyuru**
>
> YukiSU **tamamen C++ ile yeniden yazılmıştır** (önceden Rust tabanlıydı). Bu nedenle YukiSU, diğer KernelSU fork'larından farklı davranabilir. Sorun yaşarsanız lütfen üst akış projelerine değil bize bildirin.
>
> Eski Rust sürümü [`classic`](https://github.com/Anatdx/YukiSU/tree/classic) dalında durmaktadır.
>

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/Anatdx/YukiSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/manosaba)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/Anatdx/YukiSU?logo=gnu)](/LICENSE)

## Özellikler

1. Çekirdek tabanlı `su` ve root erişim yönetimi
2. Kullanıcının seçtiği modül bağlama arka uçları için harici MetaModule yaşam döngüsü ve betik entegrasyonu
   > **Not:** YukiSU, SUSFS veya yerleşik bir bağlama arka ucu içermez. Sistem modüllerini bağlamak için uyumlu bir harici MetaModule yükleyin.
3. [App Profile](https://kernelsu.org/guide/app-profile.html) ve uygulama başına non-root profil denetimleri
4. Yerleşik paket imzası yolunun dışındaki güvenilir yönetici uygulamaları için Dynamic Manager desteği
5. APatch tarzı SuperKey kimlik doğrulaması; derleme zamanı anahtarı veya `ksud` tarafından LKM içine yazılan anahtar desteklenir
6. ADB root, sulog, SELinux hide, modül `init.rc` ekleme ve güncel KernelSU userspace davranışları C++ `ksud` yığınına eşitlendi
7. arm64 LKM desteği olan TSR tabanlı sucompat/syscall hook altyapısı
8. SuperUser kaydırma eylemleri, günlük görüntüleme, soft reboot ve WebUI düzeltmeleri içeren yönetici güncellemeleri

## Uyumluluk

- YukiSU şu anda yalnızca yüklenebilir çekirdek modülü yolunu (`CONFIG_KSU=m`) destekler. Yerleşik `CONFIG_KSU=y` artık desteklenmez.

- YukiSU, Android GKI 2.0 cihazlarında (çekirdek 5.10+) LKM modunu resmen destekler. Eski ve non-GKI çekirdeklerde cihaza özel kaynak entegrasyonu gerekebilir.

- Dağıtılan tüm YukiSU bileşenleri yalnızca `arm64-v8a` hedefler. YukiZygisk,
  gelecekteki `zygote32` uygulaması için ayrıca işlevsiz `armeabi-v7a` yer
  tutucu DSO'ları ayırır; 32 bit enjeksiyon henüz desteklenmez.

## Kurulum

[`guide/installation.md`](guide/installation.md) konusuna bakın.

## Entegrasyon

[`guide/how-to-integrate.md`](guide/how-to-integrate.md) konusuna bakın.

## Sorun Giderme

1. Yönetici uygulaması kaldırılınca cihaz takılıyor mu?
   _com.sony.playmemories.mobile_ uygulamasını kaldırın.

## Sponsorlar

- [Anatdx](https://afd.anatdx.moe) (YukiSU bakımcısı)
- [ShirkNeko](https://afdian.com/a/shirkneko) (SukiSU bakımcısı)
- [weishu](https://github.com/sponsors/tiann) (KernelSU yazarı)

## Lisans

- "kernel" dizinindeki dosyalar [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) lisanslıdır.
- Bunlar dışındaki tüm kısımlar [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html) lisanslıdır.

## Katkıda Bulunanlar

- [KernelSU](https://github.com/tiann/KernelSU): Üst akış
- Modül bağlama: kullanıcı tarafından yüklenen harici MetaModule tarafından sağlanır
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
