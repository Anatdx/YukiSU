# Руководство по интеграции

YukiSU может быть интегрирован в исходные деревья ядер GKI и non-GKI, но текущий драйвер поддерживает только путь загружаемого модуля ядра (`CONFIG_KSU=m`). Встроенный `CONFIG_KSU=y` больше не поддерживается.

Некоторые настройки OEM могут привести к тому, что до 50 % кода ядра будет происходить извне дерева ядра, а не из исходного Linux или ACK. Следовательно, индивидуальные функции ядер, не относящихся к GKI, приводят к значительной фрагментации ядра, и у нас нет универсального метода для их сборки. Поэтому мы не можем предоставить загрузочные образы для ядер, не относящихся к GKI.

Предпосылка: ядро с открытым исходным кодом, способное к загрузке.

## Методы подключения

1. **TSR syscall hook:**

   - Основной путь для загружаемых модулей ядра (LKM). Поддерживается на ядрах GKI 2.0 (`5.10+`) и совместимых интеграциях из исходников.
   - Требует `CONFIG_KPROBES=y`, `CONFIG_KRETPROBES=y` и `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`.

2. **Портирование hook под конкретное устройство:**

   - [Дополнительные сведения см. в этом репозитории](https://github.com/rksuorg/kernel_patches)
   - Некоторые non-GKI ядра отключают необходимую hook-инфраструктуру или содержат значительные OEM-изменения.
   - См. [руководство по kernelsu](https://github.com/tiann/KernelSU/blob/main/website/docs/guide/how-to-integrate-for-non-gki.md#manually-modify-the-kernel-source)
   - См. [`guide/how-to-integrate.md`](how-to-integrate.md)
   - Дополнительная ссылка: [backslashxx hooks](https://github.com/backslashxx/KernelSU/issues/5)

### Как добавить драйвер ядра YukiSU в исходный код ядра

YukiSU теперь использует **единую кодовую базу** для LKM-сборок на GKI и device-specific исходных деревьях. Отдельные ветки не нужны.

```sh
curl -LSs "https://raw.githubusercontent.com/Anatdx/YukiSU/main/kernel/setup.sh" | bash -s main
```

Или укажите тег/коммит:

```sh
curl -LSs "https://raw.githubusercontent.com/Anatdx/YukiSU/main/kernel/setup.sh" | bash -s v1.2.0
```

Очистка:

```sh
curl -LSs "https://raw.githubusercontent.com/Anatdx/YukiSU/main/kernel/setup.sh" | bash -s -- --cleanup
```

### Необходимые опции конфигурации ядра

#### Для режима LKM (загружаемый модуль ядра):

```
CONFIG_KSU=m
CONFIG_KPROBES=y
CONFIG_HAVE_KPROBES=y
CONFIG_KPROBE_EVENTS=y
CONFIG_KRETPROBES=y
CONFIG_HAVE_SYSCALL_TRACEPOINTS=y
```

YukiSU не поддерживает встроенный `CONFIG_KSU=y`; выберите `m`.
