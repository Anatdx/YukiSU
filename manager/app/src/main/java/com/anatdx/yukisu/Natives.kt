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
    // minimal supported kernel version
    // 10915: allowlist breaking change, add app profile
    // 10931: app profile struct add 'version' field
    // 10946: add capabilities
    // 10977: change groups_count and groups to avoid overflow write
    // 11071: Fix the issue of failing to set a custom SELinux type.
    // 12143: breaking: new supercall impl
    const val MINIMAL_SUPPORTED_KERNEL = 10000

    // 12040: Support disable sucompat mode
    const val KERNEL_SU_DOMAIN = "u:r:su:s0"

    const val MINIMAL_SUPPORTED_KERNEL_FULL = "v1.0.0"

    const val MINIMAL_NEW_IOCTL_KERNEL = 10000

    const val ROOT_UID = 0
    const val ROOT_GID = 0

    /** root_profile.flags: block re-escalation for this profile and its children. */
    const val FLAG_KSU_NO_NEW_PRIVS = 1L

    external fun getFullVersion(): String

    /** Kernel UAPI contract version (KERNEL_SU_UAPI_VERSION); 0 if unsupported. */
    external fun getUapiVersion(): Int

    /** UAPI contract version this manager binary was built against. */
    external fun getManagerUapiVersion(): Int

    /** True when the kernel's UAPI version differs from the manager's (skew). */
    fun checkUapiMismatch(): Boolean = getUapiVersion() != getManagerUapiVersion()

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
    }

    val version: Int
        external get

    // get the uid list of allowed su processes.
    val allowList: IntArray
        external get

    /** Returns total number of apps in allow list (count only, no full list fetch). */
    external fun getSuperuserCount(): Int

    val isSafeMode: Boolean
        external get

    val isLateLoadMode: Boolean
        external get
    val isManager: Boolean
        external get

    external fun uidShouldUmount(uid: Int): Boolean
    external fun getDynamicManagers(): IntArray

    const val DYNAMIC_MANAGER_FLAG_PRESET = 1 shl 0
    const val DYNAMIC_MANAGER_FLAG_TRUSTED = 1 shl 1

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

    external fun isMagiskCompatEnabled(): Boolean
    external fun setMagiskCompatEnabled(enabled: Boolean): Boolean

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

    /**
     * ADB Root can be enabled/disabled.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isAdbRootEnabled(): Boolean
    external fun setAdbRootEnabled(enabled: Boolean): Boolean

    /**
     * SELinux Hide can be enabled/disabled.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isSelinuxHideEnabled(): Boolean
    external fun setSelinuxHideEnabled(enabled: Boolean): Boolean

    /**
     * Global "app profile 防逃逸" default. When on, the default root profile
     * (used by every profile that resolves to default, incl. the manager and
     * shell) carries NO_NEW_PRIVS. Persisted via ksud's feature config.
     */
    external fun isDefaultNoNewPrivsEnabled(): Boolean
    external fun setDefaultNoNewPrivsEnabled(enabled: Boolean): Boolean

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
    
    /**
     * Check if manager signature is considered OK by kernel.
     * This reflects whether signature-based verification is in effect.
     */
    external fun isSignatureOk(): Boolean

    /**
     * YukiZygisk injection status as a JSON string, or null when the daemon is
     * down or this process is not the kernel-authenticated manager.
     *
     * Native (jni.c): connects straight to zygiskd's abstract socket and asks
     * for a snapshot. zygiskd gates the reply with SO_PEERCRED -- it compares the
     * real uid the kernel stamped on our connection against the manager uid, so a
     * hostile app spoofing our package name can't read it. JSON shape:
     * `{ "count": Int, "recent": [appId...],
     *    "zygotes": [{"pid": Int, "name": String, "abi": String}...],
     *    "zygote_monitor": [{"pid": Int, "name": String, "abi": String,
     *      "state": String}...],
     *    "native_modules": [{"id": String, "target_type": String,
     *      "target": String, "companion": Bool, "state": String}...],
     *    "native_injections": [{"pid": Int, "process": String,
     *      "module": String, "target_type": String, "target": String,
     *      "abi": String, "companion": Bool, "state": String}...],
     *    "modules": ["name"...], "yukilinker": Bool, "denylist_mode": Int,
     *    "dmesg_log": Bool }`.
     */
    external fun yzQueryStatus(): String?
    
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
        // root_profile.flags bitmask. Neutral by default; the per-profile UI
        // seeds the anti-escape toggle from the global default
        // (isDefaultNoNewPrivsEnabled) when the profile still uses default.
        val flags: Long = 0L,

        val nonRootUseDefault: Boolean = true,
        // Default to NOT unmounting modules for non-root apps.
        // Apps without an explicit profile will keep module modifications applied.
        val umountModules: Boolean = false,
        var rules: String = "", // this field is save in ksud!!
    ) : Parcelable {
        enum class Namespace {
            INHERITED,
            GLOBAL,
            INDIVIDUAL,
        }

        constructor() : this("")
    }
}
