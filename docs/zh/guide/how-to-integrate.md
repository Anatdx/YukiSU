# 集成指导

YukiSU 可以集成到 GKI 和 non-GKI 内核中，并且已反向移植到 4.14 版本。

有些 OEM 定制可能导致多达 50% 的内核代码超出内核树代码，而非来自上游 Linux 内核或 ACK。因此，non-GKI 内核的定制特性导致了严重的内核碎片化，而且我们缺乏构建它们的通用方法。因此，我们无法提供 non-GKI 内核的启动映像。

前提条件：开源的、可启动的内核。

## Hook 方法

1. **Syscall hook:**

   - 可用于带有此钩子的可加载内核模块 (LKM) 或 GKI。 （适用于 `5.10+`）
   - 需要 `CONFIG_KSU_SYSCALL_HOOK=y` ＆ `CONFIG_KPROBES=y` ，`CONFIG_KRETPROBES=y`，`CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`

2. **Manual hook:**

   - [请参阅此存储库以获取更多信息](https://github.com/rksuorg/kernel_patches)
   - 非 GKI 内核的默认 hook 方法，`CONFIG_KPROBES` 默认情况下关闭。
   - 需要 `CONFIG_KSU_MANUAL_HOOK=y`
   - 参考 [kernelsu手册](https://github.com/tiann/KernelSU/blob/main/website/docs/guide/how-to-integrate-for-non-gki.md#manually-modify-the-kernel-source)
   - 参考 [`guide/how-to-integrate.md`](how-to-integrate.md)
   - 可选参考 [backslashxx的钩子](https://github.com/backslashxx/KernelSU/issues/5)

3. **Inline hook (HymoFS):**

   - [HymoFS](https://github.com/backslashxx/HymoFS) 是 YukiSU 内置的内核级文件系统隐藏与挂载方案
   - 使用 inline hook 拦截 VFS 操作，在内核级别隐藏文件/目录。
   - 需要 `CONFIG_KSU_HYMOFS=y`
   - 可选：`CONFIG_KSU_HYMOFS_LSMBPS=y` 启用 LSM BPF 支持

> **注意：** YukiSU 不再支持 susfs。HymoFS 是我们内置的替代方案。

### 如何将 YukiSU 内核驱动程序添加到内核源代码中

YukiSU 现在使用 **统一代码库**，同时支持 LKM 和 GKI/non-GKI 构建。不需要单独的分支。

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
```

#### GKI（内置）模式：

```
CONFIG_KSU=y
CONFIG_KPROBES=y
CONFIG_HAVE_KPROBES=y
CONFIG_KPROBE_EVENTS=y
```

#### 可选功能：

```
# 启用 HymoFS（内核级文件系统隐藏）
CONFIG_KSU_HYMOFS=y

# 启用 HymoFS LSM BPF 支持（需要 LSM BPF）
CONFIG_KSU_HYMOFS_LSMBPS=y

# 启用 KPM（KernelPatch 模块）支持
CONFIG_KPM=y
```
