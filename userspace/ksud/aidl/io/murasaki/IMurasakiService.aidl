// IMurasakiService.aidl
// Murasaki - KernelSU Kernel-level API
//
// AIDL 接口定义，使用 libbinder_ndk 稳定 ABI

package io.murasaki;

import io.murasaki.IHymoFsService;
import io.murasaki.IKernelService;

interface IMurasakiService {
    // 版本信息
    int getVersion();
    int getKsuVersion();
    
    // 权限等级: 0=SHELL, 1=ROOT, 2=KERNEL
    int getPrivilegeLevel();
    boolean isKernelModeAvailable();
    
    // SELinux
    String getSelinuxContext(int pid);
    
    // 获取子服务
    IHymoFsService getHymoFsService();
    IKernelService getKernelService();
    
    // 进程执行 (Shizuku 兼容)
    ParcelFileDescriptor newProcess(in String[] cmd, in String[] env, in String dir);
    
    // 权限检查
    int checkPermission(String permission);
}
