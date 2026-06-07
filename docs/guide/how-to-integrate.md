# Integration Guidance

YukiSU can be integrated into GKI and non-GKI kernel source trees, but the current driver supports only the loadable kernel module path (`CONFIG_KSU=m`). Built-in `CONFIG_KSU=y` is no longer supported.

Certain OEM customisations may result in up to 50% of kernel code originating outside the kernel tree, rather than from upstream Linux or ACK. Consequently, the bespoke features of non-GKI kernels cause significant kernel fragmentation, and we lack a universal method for building them. Therefore, we cannot provide boot images for non-GKI kernels.

Prerequisite: An open-source, bootable kernel.

## Hook Methods

1. **TSR syscall hook:**

   - Default path for loadable kernel modules (LKM). Supported on GKI 2.0 kernels (`5.10+`) and compatible source-integrated kernels.
   - Requires `CONFIG_KPROBES=y`, `CONFIG_KRETPROBES=y`, and `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`.

2. **Device-specific hook porting:**

   - [Refer to this repository for further details](https://github.com/rksuorg/kernel_patches)
   - Some non-GKI kernels disable the required hook infrastructure or carry heavy OEM changes.
   - Refer to the [kernelsu manual](https://github.com/tiann/KernelSU/blob/main/website/docs/guide/how-to-integrate-for-non-gki.md#manually-modify-the-kernel-source)
   - Refer to [`guide/how-to-integrate.md`](how-to-integrate.md)
   - Optional reference: [backslashxx hooks](https://github.com/backslashxx/KernelSU/issues/5)

### How to add the YukiSU kernel driver to the kernel source code

YukiSU now uses a **unified codebase** for LKM builds across GKI and device-specific source trees. No separate branches are needed.

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
CONFIG_KRETPROBES=y
CONFIG_HAVE_SYSCALL_TRACEPOINTS=y
```

YukiSU does not support built-in `CONFIG_KSU=y`; choose `m`.
