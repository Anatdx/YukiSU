// IKernelService.aidl
// KernelSU 内核操作接口

package io.murasaki;

interface IKernelService {
    // App Profile
    String getAppProfile(int uid);
    int setAppProfile(int uid, String profileJson);
    
    // Root 权限查询
    boolean isUidGrantedRoot(int uid);
    boolean shouldUmountForUid(int uid);
    
    // SEPolicy
    int injectSepolicy(String rules);
    int loadSepolicyFromFile(String path);
    
    // 高级操作 (需要 KERNEL 权限)
    int addTryUmount(String path);
    int nukeExt4Sysfs();
    
    // 原始 ioctl (需要 KERNEL 权限)
    int rawIoctl(int cmd, in byte[] data, out byte[] result);
}
