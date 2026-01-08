package com.anatdx.yukisu

import android.os.Parcelable
import androidx.annotation.Keep
import androidx.compose.runtime.Immutable
import kotlinx.parcelize.Parcelize

/**
 * @author weishu
 * @date 2022/12/8.
 */
object Natives {
    const val MINIMAL_SUPPORTED_KERNEL = 10000

    const val KERNEL_SU_DOMAIN = "u:r:su:s0"

    const val MINIMAL_SUPPORTED_KERNEL_FULL = "v1.0.0"

    const val MINIMAL_SUPPORTED_KPM = 10000

    const val MINIMAL_NEW_IOCTL_KERNEL = 10000

    const val ROOT_UID = 0
    const val ROOT_GID = 0

    // 获取完整版本号
    external fun getFullVersion(): String

    fun isVersionLessThan(v1Full: String, v2Full: String): Boolean {
        fun extractVersionParts(version: String): List<Int> {
            val match = Regex("""v\d+(\.\d+)*""").find(version)
            val simpleVersion = match?.value ?: version
            return simpleVersion.trimStart('v').split('.').map { it.toIntOrNull() ?: 0 }
        }

        val v1Parts = extractVersionParts(v1Full)
        val v2Parts = extractVersionParts(v2Full)
        val maxLength = maxOf(v1Parts.size, v2Parts.size)
        for (i in 0 until maxLength) {
            val num1 = v1Parts.getOrElse(i) { 0 }
            val num2 = v2Parts.getOrElse(i) { 0 }
            if (num1 != num2) return num1 < num2
        }
        return false
    }

    fun getSimpleVersionFull(): String = getFullVersion().let { version ->
        Regex("""v\d+(\.\d+)*""").find(version)?.value ?: version
    }

    init {
        System.loadLibrary("kernelsu")
        try {
            System.loadLibrary("murasaki_binder")
        } catch (e: UnsatisfiedLinkError) {
            // Binder library may not be available
        }
    }

    val version: Int
        external get

    // get the uid list of allowed su processes.
    val allowList: IntArray
        external get

    val isSafeMode: Boolean
        external get

    val isLkmMode: Boolean
        external get

    val isManager: Boolean
        external get

    external fun uidShouldUmount(uid: Int): Boolean

    /**
     * Get the profile of the given package.
     * @param key usually the package name
     * @return return null if failed.
     */
    external fun getAppProfile(key: String?, uid: Int): Profile
    external fun setAppProfile(profile: Profile?): Boolean

    /**
     * `su` compat mode can be disabled temporarily.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isSuEnabled(): Boolean
    external fun setSuEnabled(enabled: Boolean): Boolean

    /**
     * Kernel module umount can be disabled temporarily.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isKernelUmountEnabled(): Boolean
    external fun setKernelUmountEnabled(enabled: Boolean): Boolean

    /**
     * Enhanced security can be enabled/disabled.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isEnhancedSecurityEnabled(): Boolean
    external fun setEnhancedSecurityEnabled(enabled: Boolean): Boolean

    /**
     * Su Log can be enabled/disabled.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isSuLogEnabled(): Boolean
    external fun setSuLogEnabled(enabled: Boolean): Boolean

    external fun isKPMEnabled(): Boolean
    external fun getHookType(): String

    external fun getUserName(uid: Int): String?

    /**
     * SuperKey authentication
     * Authenticates the manager with the given SuperKey
     * @param superKey the secret key to authenticate
     * @return true if authentication successful, false otherwise
     */
    external fun authenticateSuperKey(superKey: String): Boolean
    
    /**
     * Check if KSU driver is present (without authentication)
     * @return true if driver fd can be found, false otherwise
     */
    external fun isKsuDriverPresent(): Boolean
    
    /**
     * Check if SuperKey is configured in kernel
     * @return true if SuperKey is configured, false otherwise
     */
    external fun isSuperKeyConfigured(): Boolean
    
    /**
     * Check if already authenticated via SuperKey
     * @return true if authenticated via SuperKey, false otherwise
     */
    external fun isSuperKeyAuthenticated(): Boolean
    
    private const val NON_ROOT_DEFAULT_PROFILE_KEY = "$"
    private const val NOBODY_UID = 9999

    fun setDefaultUmountModules(umountModules: Boolean): Boolean {
        Profile(
            NON_ROOT_DEFAULT_PROFILE_KEY,
            NOBODY_UID,
            false,
            umountModules = umountModules
        ).let {
            return setAppProfile(it)
        }
    }

    fun isDefaultUmountModules(): Boolean {
        getAppProfile(NON_ROOT_DEFAULT_PROFILE_KEY, NOBODY_UID).let {
            return it.umountModules
        }
    }

    fun requireNewKernel(): Boolean {
        if (version != -1 && version < MINIMAL_SUPPORTED_KERNEL) return true
        return isVersionLessThan(getFullVersion(), MINIMAL_SUPPORTED_KERNEL_FULL)
    }

    @Immutable
    @Parcelize
    @Keep
    data class Profile(
        // and there is a default profile for root and non-root
        val name: String,
        // current uid for the package, this is convivent for kernel to check
        // if the package name doesn't match uid, then it should be invalidated.
        val currentUid: Int = 0,

        // if this is true, kernel will grant root permission to this package
        val allowSu: Boolean = false,

        // these are used for root profile
        val rootUseDefault: Boolean = true,
        val rootTemplate: String? = null,
        val uid: Int = ROOT_UID,
        val gid: Int = ROOT_GID,
        val groups: List<Int> = mutableListOf(),
        val capabilities: List<Int> = mutableListOf(),
        val context: String = KERNEL_SU_DOMAIN,
        val namespace: Int = Namespace.INHERITED.ordinal,

        val nonRootUseDefault: Boolean = true,
        val umountModules: Boolean = true,
        var rules: String = "", // this field is save in ksud!!
    ) : Parcelable {
        enum class Namespace {
            INHERITED,
            GLOBAL,
            INDIVIDUAL,
        }

        constructor() : this("")
    }

    // ==================== Murasaki Binder API ====================
    
    /**
     * Check if connected to Murasaki Binder service
     */
    @JvmStatic
    external fun murasakiBinderConnected(): Boolean
    
    /**
     * Disconnect from Murasaki Binder service
     */
    @JvmStatic
    external fun murasakiBinderDisconnect()
    
    /**
     * Get Murasaki service version
     */
    @JvmStatic
    external fun murasakiGetVersion(): Int
    
    /**
     * Get KernelSU kernel version via Binder
     */
    @JvmStatic
    external fun murasakiGetKsuVersion(): Int
    
    /**
     * Get current privilege level
     * 0=SHELL, 1=ROOT, 2=KERNEL
     */
    @JvmStatic
    external fun murasakiGetPrivilegeLevel(): Int
    
    /**
     * Check if kernel mode (HymoFS) is available
     */
    @JvmStatic
    external fun murasakiIsKernelModeAvailable(): Boolean
    
    /**
     * Get SELinux context for a process
     */
    @JvmStatic
    external fun murasakiGetSelinuxContext(pid: Int): String
    
    // HymoFS operations
    
    /**
     * Add a hide rule for a path
     */
    @JvmStatic
    external fun murasakiHymoAddHideRule(path: String, targetUid: Int): Int
    
    /**
     * Add a redirect rule
     */
    @JvmStatic
    external fun murasakiHymoAddRedirectRule(src: String, target: String, targetUid: Int): Int
    
    /**
     * Clear all HymoFS rules
     */
    @JvmStatic
    external fun murasakiHymoClearRules(): Int
    
    /**
     * Set stealth mode
     */
    @JvmStatic
    external fun murasakiHymoSetStealthMode(enable: Boolean): Int
    
    /**
     * Set debug mode
     */
    @JvmStatic
    external fun murasakiHymoSetDebugMode(enable: Boolean): Int
    
    /**
     * Get active HymoFS rules
     */
    @JvmStatic
    external fun murasakiHymoGetActiveRules(): String
    
    // Kernel operations
    
    /**
     * Check if UID is granted root
     */
    @JvmStatic
    external fun murasakiIsUidGrantedRoot(uid: Int): Boolean
    
    /**
     * Nuke ext4 sysfs entries
     */
    @JvmStatic
    external fun murasakiNukeExt4Sysfs(): Int
}