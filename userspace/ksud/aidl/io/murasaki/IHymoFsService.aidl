// IHymoFsService.aidl
// HymoFS 文件系统操作接口

package io.murasaki;

interface IHymoFsService {
    // 规则管理
    int addHideRule(String path, int uid);
    int addRedirectRule(String src, String target, int uid);
    int removeRule(int ruleId);
    int clearRules();
    
    // 隐身模式
    int setStealthMode(boolean enable);
    boolean isStealthMode();
    
    // 调试
    int setDebugMode(boolean enable);
    boolean isDebugMode();
    
    // 配置
    int setMirrorPath(String path);
    String getMirrorPath();
    
    // 挂载修复
    int fixMounts();
    
    // 状态查询
    String getActiveRules();
    String getStatus();
}
