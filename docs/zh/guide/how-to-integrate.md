# 集成指导

YukiSU 可以集成到 GKI 和 non-GKI 内核源码树中，但当前驱动仅支持可加载内核模块路径（`CONFIG_KSU=m`），不再支持内置 `CONFIG_KSU=y`。

有些 OEM 定制可能导致多达 50% 的内核代码超出内核树代码，而非来自上游 Linux 内核或 ACK。因此，non-GKI 内核的定制特性导致了严重的内核碎片化，而且我们缺乏构建它们的通用方法。因此，我们无法提供 non-GKI 内核的启动映像。

前提条件：开源的、可启动的内核。

## Hook 方法

1. **TSR syscall hook:**

   - 可加载内核模块 (LKM) 的默认路径。适用于 GKI 2.0 内核（`5.10+`）以及兼容的源码集成内核。
   - 需要 `CONFIG_KPROBES=y`、`CONFIG_KRETPROBES=y` 与 `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`。

2. **按设备移植 hook:**

   - [请参阅此存储库以获取更多信息](https://github.com/rksuorg/kernel_patches)
   - 部分 non-GKI 内核会关闭所需 hook 基础设施，或带有较重的 OEM 改动。
   - 参考 [kernelsu手册](https://github.com/tiann/KernelSU/blob/main/website/docs/guide/how-to-integrate-for-non-gki.md#manually-modify-the-kernel-source)
   - 参考 [`guide/how-to-integrate.md`](how-to-integrate.md)
   - 可选参考 [backslashxx的钩子](https://github.com/backslashxx/KernelSU/issues/5)

### 如何将 YukiSU 内核驱动程序添加到内核源代码中

YukiSU 现在使用 **统一代码库**，面向 GKI 与按设备源码集成的 LKM 构建。不需要单独的分支。

```sh
curl -LSs "https://raw.githubusercontent.com/Anatdx/YukiSU/main/kernel/setup.sh" | bash -s main
```

或指定标签/提交：

```sh
curl -LSs "https://raw.githubusercontent.com/Anatdx/YukiSU/main/kernel/setup.sh" | bash -s v1.2.0
```

清理：

```sh
curl -LSs "https://raw.githubusercontent.com/Anatdx/YukiSU/main/kernel/setup.sh" | bash -s -- --cleanup
```

### 所需的内核配置选项

#### LKM（可加载内核模块）模式：

```
CONFIG_KSU=m
CONFIG_KPROBES=y
CONFIG_HAVE_KPROBES=y
CONFIG_KPROBE_EVENTS=y
CONFIG_KRETPROBES=y
CONFIG_HAVE_SYSCALL_TRACEPOINTS=y
```

YukiSU 不支持内置 `CONFIG_KSU=y`；请选择 `m`。
