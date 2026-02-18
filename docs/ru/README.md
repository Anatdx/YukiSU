# YukiSU

<img align='right' src='../YukiSU-mini.svg' width='220px' alt="yukisu logo">


[English](../README.md) | [简体中文](../zh/README.md) | [日本語](../ja/README.md) | [Türkçe](../tr/README.md) | **Русский**

Решение для получения root на основе ядра для устройств Android: форк [`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra) с удалением лишнего и рядом изменений.

> **⚠️ Важное уведомление**
>
> YukiSU **полностью переписан на C++** (ранее был на Rust). Поэтому поведение YukiSU может отличаться от других форков KernelSU. При проблемах сообщайте нам, а не в вышестоящие проекты.
>
> Классическая версия на Rust сохранена в ветке [`classic`](https://github.com/Anatdx/YukiSU/tree/classic).
>

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/tiann/KernelSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/hymo_chat)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/tiann/KernelSU?logo=gnu)](/LICENSE)

## Возможности

1. Управление `su` и root-доступом на основе ядра
2. Система модулей на базе [Magic Mount](https://github.com/5ec1cff/KernelSU)
   > **Примечание:** YukiSU передаёт все операции монтирования модулей установленному *metamodule*; ядро больше не выполняет монтирование.
3. [App Profile](https://kernelsu.org/guide/app-profile.html): ограничение root-доступа
4. Настройка темы менеджера

## Совместимость

- YukiSU официально поддерживает устройства с Android GKI 2.0 (ядро 5.10+).

- Старые ядра (4.4+) также поддерживаются, но ядро нужно собирать вручную.

- При дополнительных бэкпортах возможна поддержка ядра 3.x (3.4–3.18).

- Сейчас поддерживаются только `arm64-v8a`, `armeabi-v7a (bare)` и `X86_64` (частично).

## Установка

См. [`guide/installation.md`](guide/installation.md)

## Интеграция

См. [`guide/how-to-integrate.md`](guide/how-to-integrate.md)

## Перевод

Чтобы предложить перевод менеджера, перейдите на [Crowdin](https://crowdin.com/project/YukiSU).

## Устранение неполадок

1. Устройство зависает после удаления приложения менеджера?
   Удалите _com.sony.playmemories.mobile_

## Спонсоры

- [ShirkNeko](https://afdian.com/a/shirkneko) (мейнтейнер SukiSU)
- [weishu](https://github.com/sponsors/tiann) (автор KernelSU)

## Лицензия

- Файлы в каталоге «kernel» распространяются под [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).
- Всё остальное — под [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html).

## Благодарности

- [KernelSU](https://github.com/tiann/KernelSU): исходный проект
- [MKSU](https://github.com/5ec1cff/KernelSU): Magic Mount
- [RKSU](https://github.com/rsuntk/KernelsU): поддержка non-GKI
- [KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch — ключевая часть реализации модуля ядра APatch

<details>
<summary>Благодарности KernelSU</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/): идея KernelSU
- [Magisk](https://github.com/topjohnwu/Magisk): инструмент для root
- [genuine](https://github.com/brevent/genuine/): проверка подписи APK v2
- [Diamorphine](https://github.com/m0nad/Diamorphine): приёмы rootkit
</details>
