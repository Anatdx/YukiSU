package io.murasaki

import com.anatdx.yukisu.Natives

/**
 * Murasaki Native 接口
 * 
 * 与 ksud 的 Murasaki 服务通信
 * 优先使用 Binder，回退到 Unix Socket
 */
internal object MurasakiNative {
    
    private var useBinderApi = false
    
    init {
        System.loadLibrary("kernelsu")
        try {
            System.loadLibrary("murasaki_binder")
            useBinderApi = true
        } catch (e: UnsatisfiedLinkError) {
            useBinderApi = false
        }
    }
    
    fun isBinderAvailable(): Boolean = useBinderApi && Natives.murasakiBinderConnected()
    
    // 基础信息
    fun getVersion(): Int = if (isBinderAvailable()) {
        Natives.murasakiGetVersion()
    } else {
        nativeGetVersion()
    }
    
    fun getKsuVersion(): Int = if (isBinderAvailable()) {
        Natives.murasakiGetKsuVersion()
    } else {
        nativeGetKsuVersion()
    }
    
    fun getPrivilegeLevel(): Int = if (isBinderAvailable()) {
        Natives.murasakiGetPrivilegeLevel()
    } else {
        nativeGetPrivilegeLevel()
    }
    
    fun isKernelModeAvailable(): Boolean = if (isBinderAvailable()) {
        Natives.murasakiIsKernelModeAvailable()
    } else {
        nativeIsKernelModeAvailable()
    }
    
    // SELinux
    fun getSelinuxContext(pid: Int): String? = if (isBinderAvailable()) {
        Natives.murasakiGetSelinuxContext(pid).takeIf { it.isNotEmpty() }
    } else {
        nativeGetSelinuxContext(pid)
    }
    
    // HymoFS
    fun hymoAddRule(src: String, target: String, type: Int): Int = if (isBinderAvailable()) {
        if (type == 0) {
            Natives.murasakiHymoAddRedirectRule(src, target, 0)
        } else {
            Natives.murasakiHymoAddHideRule(src, 0)
        }
    } else {
        nativeHymoAddRule(src, target, type)
    }
    
    fun hymoClearRules(): Int = if (isBinderAvailable()) {
        Natives.murasakiHymoClearRules()
    } else {
        nativeHymoClearRules()
    }
    
    fun hymoSetStealth(enable: Boolean): Int = if (isBinderAvailable()) {
        Natives.murasakiHymoSetStealthMode(enable)
    } else {
        nativeHymoSetStealth(enable)
    }
    
    fun hymoSetDebug(enable: Boolean): Int = if (isBinderAvailable()) {
        Natives.murasakiHymoSetDebugMode(enable)
    } else {
        nativeHymoSetDebug(enable)
    }
    
    fun hymoGetActiveRules(): String? = if (isBinderAvailable()) {
        Natives.murasakiHymoGetActiveRules().takeIf { it.isNotEmpty() }
    } else {
        nativeHymoGetActiveRules()
    }
    
    // KSU
    fun isUidGrantedRoot(uid: Int): Boolean = if (isBinderAvailable()) {
        Natives.murasakiIsUidGrantedRoot(uid)
    } else {
        nativeIsUidGrantedRoot(uid)
    }
    
    fun nukeExt4Sysfs(): Int = if (isBinderAvailable()) {
        Natives.murasakiNukeExt4Sysfs()
    } else {
        nativeNukeExt4Sysfs()
    }
    
    // 连接管理
    fun disconnect() {
        if (useBinderApi) {
            Natives.murasakiBinderDisconnect()
        }
        nativeDisconnect()
    }
    
    // Legacy native methods (Unix Socket fallback)
    private external fun nativeGetVersion(): Int
    private external fun nativeGetKsuVersion(): Int
    private external fun nativeGetPrivilegeLevel(): Int
    private external fun nativeIsKernelModeAvailable(): Boolean
    private external fun nativeGetSelinuxContext(pid: Int): String?
    private external fun nativeHymoAddRule(src: String, target: String, type: Int): Int
    private external fun nativeHymoClearRules(): Int
    private external fun nativeHymoSetStealth(enable: Boolean): Int
    private external fun nativeHymoSetDebug(enable: Boolean): Int
    private external fun nativeHymoSetMirrorPath(path: String): Int
    private external fun nativeHymoFixMounts(): Int
    private external fun nativeHymoGetActiveRules(): String?
    private external fun nativeIsUidGrantedRoot(uid: Int): Boolean
    private external fun nativeNukeExt4Sysfs(): Int
    private external fun nativeDisconnect()
}
