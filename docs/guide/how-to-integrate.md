# Integration Guidance

YukiSU can be integrated into both GKI and non-GKI kernels and has been backported to version 4.14.

Certain OEM customisations may result in up to 50% of kernel code originating outside the kernel tree, rather than from upstream Linux or ACK. Consequently, the bespoke features of non-GKI kernels cause significant kernel fragmentation, and we lack a universal method for building them. Therefore, we cannot provide boot images for non-GKI kernels.

Prerequisite: An open-source, bootable kernel.

## Hook Methods

1. **Syscall hook:**

   - Applicable to loadable kernel modules (LKM) or GKI with this hook. (Supported in `5.10+`)
   - Requires `CONFIG_KSU_SYSCALL_HOOK=y` & `CONFIG_KPROBES=y`, `CONFIG_KRETPROBES=y`, `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`

2. **Manual hook:**

   - [Refer to this repository for further details](https://github.com/rksuorg/kernel_patches)
   - Default hook method for non-GKI kernels; `CONFIG_KPROBES` is disabled by default.
   - Requires `CONFIG_KSU_MANUAL_HOOK=y`
   - Refer to the [kernelsu manual](https://github.com/tiann/KernelSU/blob/main/website/docs/guide/how-to-integrate-for-non-gki.md#manually-modify-the-kernel-source)
   - Refer to [`guide/how-to-integrate.md`](how-to-integrate.md)
   - Optional reference: [backslashxx hooks](https://github.com/backslashxx/KernelSU/issues/5)

### How to add the YukiSU kernel driver to the kernel source code

YukiSU now uses a **unified codebase** for both LKM and GKI/non-GKI builds. No separate branches are needed.

```sh
curl -LSs "https://raw.githubusercontent.com/Anatdx/YukiSU/main/kernel/setup.sh" | bash -s main
```

Or specify a tag/commit:

```sh
curl -LSs "https://raw.githubusercontent.com/Anatdx/YukiSU/main/kernel/setup.sh" | bash -s v1.2.0
```

To cleanup:

```sh
curl -LSs "https://raw.githubusercontent.com/Anatdx/YukiSU/main/kernel/setup.sh" | bash -s -- --cleanup
```

### Required Kernel Config Options

#### For LKM (Loadable Kernel Module) mode:

```
CONFIG_KSU=m
CONFIG_KPROBES=y
CONFIG_HAVE_KPROBES=y
CONFIG_KPROBE_EVENTS=y
```

#### For GKI (built-in) mode:

```
CONFIG_KSU=y
CONFIG_KPROBES=y
CONFIG_HAVE_KPROBES=y
CONFIG_KPROBE_EVENTS=y
```
