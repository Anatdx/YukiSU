# YukiSU

<img align='right' src='YukiSU-mini.svg' width='250px' alt="yukisu logo">


[English](../README.md) | [简体中文](../zh/README.md) | [日本語](../ja/README.md) | **Türkçe** | [Русский](../ru/README.md)

Android cihazlar için çekirdek tabanlı root çözümü, [`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra)'dan fork edilmiş, gereksiz şeyler kaldırılmış ve ilginç değişiklikler eklenmiştir.

> **⚠️ Önemli Duyuru**
>
> YukiSU **tamamen C++ ile yeniden yazılmıştır** (önceden Rust tabanlıydı). Bu yeniden yazım, YukiSU'nun diğer KernelSU forklarından farklı davranabileceği anlamına gelir. Herhangi bir sorunla karşılaşırsanız, lütfen üst akış projelerine değil bize bildirin.
>
> Klasik Rust sürümü [`classic`](https://github.com/Anatdx/YukiSU/tree/classic) dalında korunmaktadır.
>
> Sürüm **1.2.0**'den itibaren, çekirdek sürücüsü hem LKM hem de GKI/GKI olmayan derlemeler için **birleşik kod tabanı** kullanmaktadır.

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/tiann/KernelSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/hymo_chat)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/tiann/KernelSU?logo=gnu)](/LICENSE)

## Özellikler

1. Çekirdek tabanlı `su` ve root erişim yönetimi
2. [Magic Mount](https://github.com/5ec1cff/KernelSU) tabanlı modül sistemi
   > **Not:** YukiSU artık tüm modül bağlama işlemlerini yüklü *metamodule*'e devretmektedir; çekirdek artık bağlama işlemlerini işlememektedir.
3. [App Profile](https://kernelsu.org/guide/app-profile.html): Root yetkilerini kafese kilitleyin
4. GKI olmayan ve GKI 1.0 desteği
5. KPM Desteği
6. Çekirdek düzeyinde dosya sistemi gizleme için dahili [HymoFS](https://github.com/backslashxx/HymoFS) (susfs'in yerini alır)
7. Yönetici teması ayarlamaları

> **Not:** YukiSU artık susfs'i desteklememektedir. HymoFS, dosya sistemi gizleme için yerleşik çözümümüzdür.

## Uyumluluk Durumu

- YukiSU, Android GKI 2.0 cihazlarını (çekirdek 5.10+) resmi olarak destekler.

- Eski çekirdekler (4.4+) de uyumludur, ancak çekirdek manuel olarak derlenmelidir.

- Daha fazla geri taşımayla YukiSU, 3.x çekirdeğini (3.4-3.18) destekleyebilir.

- Şu anda yalnızca `arm64-v8a`, `armeabi-v7a (bare)` ve `X86_64` (bazıları) desteklenmektedir.

## Kurulum

[`guide/installation.md`](guide/installation.md) dosyasına bakın

## Entegrasyon

[`guide/how-to-integrate.md`](guide/how-to-integrate.md) dosyasına bakın

## Çeviri

Yönetici için çeviri göndermek istiyorsanız, lütfen [Crowdin](https://crowdin.com/project/YukiSU)'a gidin.

## KPM Desteği

- KernelPatch tabanlı olarak KSU ile gereksiz işlevleri kaldırdık ve yalnızca KPM desteğini koruduk.
- Devam eden çalışma: Farklı uygulamalar arasında uyumluluğu sağlamak için ek işlevler entegre ederek APatch uyumluluğunu genişletiyoruz.

**Açık kaynak deposu**: [https://github.com/ShirkNeko/YukiSU_KernelPatch_patch](https://github.com/ShirkNeko/YukiSU_KernelPatch_patch)

**KPM şablonu**: [https://github.com/udochina/KPM-Build-Anywhere](https://github.com/udochina/KPM-Build-Anywhere)

> [!Note]
>
> 1. `CONFIG_KPM=y` gerektirir
> 2. GKI olmayan cihazlar `CONFIG_KALLSYMS=y` ve `CONFIG_KALLSYMS_ALL=y` gerektirir
> 3. `4.19`'un altındaki çekirdekler için `4.19`'dan `set_memory.h` geri taşıma gerektirir

## Sorun Giderme

1. Yönetici uygulaması kaldırılınca cihaz takılıyor mu?
   _com.sony.playmemories.mobile_'ı kaldırın

## Sponsorlar

- [ShirkNeko](https://afdian.com/a/shirkneko) (SukiSU bakımcısı)
- [weishu](https://github.com/sponsors/tiann) (KernelSU yazarı)

## Lisans

- `kernel` dizinindeki dosyalar [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) lisansı altındadır.
- Yukarıda belirtilen dosyalar veya dizinler hariç, diğer tüm parçalar [GPL-3.0 veya üzeri](https://www.gnu.org/licenses/gpl-3.0.html) lisansı altındadır.

## Katkıda Bulunanlar

- [KernelSU](https://github.com/tiann/KernelSU): Üst akış
- [MKSU](https://github.com/5ec1cff/KernelSU): Magic Mount
- [RKSU](https://github.com/rsuntk/KernelsU): GKI olmayan destek
- [HymoFS](https://github.com/backslashxx/HymoFS): Çekirdek düzeyinde dosya sistemi gizleme
- [KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch, APatch çekirdek modülü uygulamasının önemli bir parçasıdır

<details>
<summary>KernelSU'nun katkıda bulunanları</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/): KernelSU fikri
- [Magisk](https://github.com/topjohnwu/Magisk): Güçlü root aracı
- [genuine](https://github.com/brevent/genuine/): APK v2 imza doğrulama
- [Diamorphine](https://github.com/m0nad/Diamorphine): Rootkit becerileri
</details>
