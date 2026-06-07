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

[![Latest release](https://img.shields.io/github/v/release/Anatdx/YukiSU?label=Release&logo=github)](https://github.com/Anatdx/YukiSU/releases/latest)
[![Channel](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/manosaba)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub License](https://img.shields.io/github/license/Anatdx/YukiSU?logo=gnu)](/LICENSE)

## Возможности

1. Управление `su` и root-доступом на основе ядра
2. Встроенный гибридный модульный бэкенд монтирования Kasumi, заменяющий старый путь HymoFS
   > **Примечание:** YukiSU больше не поддерживает SUSFS. В названиях внешних пакетов ядра SUSFS может оставаться, но сам YukiSU использует Kasumi.
3. [App Profile](https://kernelsu.org/guide/app-profile.html) и по-приложенческие профили non-root
4. Dynamic Manager для доверенных приложений менеджера за пределами встроенного пути подписи пакета
5. Аутентификация SuperKey в стиле APatch: ключ может задаваться при сборке или записываться в LKM через `ksud`
6. ADB root, sulog, SELinux hide, внедрение модульного `init.rc` и актуальное поведение KernelSU userspace синхронизированы со стеком C++ `ksud`
7. Инфраструктура sucompat/syscall hook на базе TSR с поддержкой arm64 и x86_64 LKM
8. Обновления менеджера: настройка Kasumi, свайпы SuperUser, просмотр логов, soft reboot и исправления WebUI

## Совместимость

- YukiSU сейчас поддерживает только путь загружаемого модуля ядра (`CONFIG_KSU=m`). Встроенный `CONFIG_KSU=y` больше не поддерживается.

- YukiSU официально поддерживает режим LKM на устройствах Android GKI 2.0 (ядро 5.10+). Для старых и non-GKI ядер может потребоваться интеграция исходников под конкретное устройство.

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

- [Anatdx](https://afd.anatdx.moe) (мейнтейнер YukiSU)
- [ShirkNeko](https://afdian.com/a/shirkneko) (мейнтейнер SukiSU)
- [weishu](https://github.com/sponsors/tiann) (автор KernelSU)

## Лицензия

- Файлы в каталоге «kernel» распространяются под [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).
- Всё остальное — под [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html).

## Благодарности

- [KernelSU](https://github.com/tiann/KernelSU): исходный проект
- Kasumi: встроенный гибридный бэкенд монтирования
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
