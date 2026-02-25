package com.anatdx.yukisu.ui.hymofs.util

import android.util.Log
import com.anatdx.yukisu.ksuApp
import com.anatdx.yukisu.ui.util.getRootShell
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.json.JSONArray
import java.io.File

/**
 * HymoFS Manager - Interfaces with ksud hymo commands
 */
object HymoFSManager {
    private const val TAG = "HymoFSManager"
    
    // Paths
    const val HYMO_CONFIG_DIR = "/data/adb/hymo"
    const val HYMO_CONFIG_FILE = "/data/adb/hymo/config.json"
    const val HYMO_STATE_FILE = "/data/adb/hymo/run/daemon_state.json"
    const val HYMO_LOG_FILE = "/data/adb/hymo/daemon.log"
    const val MODULE_DIR = "/data/adb/modules"
    const val DISABLE_BUILTIN_MOUNT_FILE = "/data/adb/ksu/.disable_builtin_mount"
    
    /**
     * Get ksud path
     */
    private fun getKsud(): String {
        return ksuApp.applicationInfo.nativeLibraryDir + File.separator + "libksud.so"
    }
    
    /**
     * HymoFS status enum (codes only; UI text is i18n)
     */
    enum class HymoFSStatus(val code: Int) {
        AVAILABLE(0),
        NOT_PRESENT(1),
        KERNEL_TOO_OLD(2),
        MODULE_TOO_OLD(3);
        
        companion object {
            fun fromCode(code: Int): HymoFSStatus = entries.find { it.code == code } ?: NOT_PRESENT
        }
    }
    
    /**
     * Module info data class
     */
    data class ModuleRule(
        val path: String,
        val mode: String
    )

    data class ModuleInfo(
        val id: String,
        val name: String,
        val version: String,
        val author: String,
        val description: String,
        val mode: String,    // auto, hymofs, overlay, magic, none
        val strategy: String, // resolved strategy: hymofs, overlay, magic
        val path: String,
        val enabled: Boolean = true,
        val rules: List<ModuleRule> = emptyList()
    )
    
    /**
     * Mount statistics (aligned with WebUI api system)
     */
    data class MountStats(
        val totalMounts: Int,
        val successfulMounts: Int,
        val failedMounts: Int,
        val tmpfsCreated: Int,
        val filesMounted: Int,
        val dirsMounted: Int,
        val symlinksCreated: Int,
        val overlayfsMounts: Int,
        val successRate: Double?
    )

    /**
     * Partition info (aligned with WebUI detectedPartitions)
     */
    data class PartitionInfo(
        val name: String,
        val mountPoint: String,
        val fsType: String,
        val isReadOnly: Boolean,
        val existsAsSymlink: Boolean
    )

    /**
     * System info data class (aligned with WebUI SystemInfo)
     */
    data class SystemInfo(
        val kernel: String,
        val selinux: String,
        val mountBase: String,
        val activeMounts: List<String>,
        val hymofsModuleIds: List<String>,
        val hymofsMismatch: Boolean,
        val mismatchMessage: String?,
        val mountStats: MountStats? = null,
        val detectedPartitions: List<PartitionInfo> = emptyList()
    )
    
    /**
     * Storage info data class
     */
    data class StorageInfo(
        val size: String,
        val used: String,
        val avail: String,
        val percent: String,
        val type: String  // tmpfs, ext4, hymofs
    )
    
    /**
     * Config data class (aligned with hymo Config)
     */
    data class HymoConfig(
        val moduledir: String = MODULE_DIR,
        val tempdir: String = "",
        val mountsource: String = "KSU",
        val debug: Boolean = false,
        val verbose: Boolean = false,
        val fsType: String = "auto",              // "auto", "ext4", "erofs", "tmpfs"
        val disableUmount: Boolean = false,
        val enableNuke: Boolean = true,
        val ignoreProtocolMismatch: Boolean = false,
        val enableKernelDebug: Boolean = false,
        val enableStealth: Boolean = true,
        val hymofsEnabled: Boolean = true,
        val mirrorPath: String = "",
        val partitions: List<String> = emptyList(),
        val unameRelease: String = "",
        val unameVersion: String = "",
        val lkmAutoload: Boolean = true,
        val lkmKmiOverride: String = "",
        val hymofsBuiltin: Boolean = false
    )
    
    /**
     * Active rule from kernel
     */
    data class ActiveRule(
        val type: String,  // add, hide, inject, merge, hide_xattr_sb
        val src: String,
        val target: String? = null,
        val extra: Int? = null
    )

    private fun extractJsonObject(text: String): JSONObject? {
        val start = text.indexOf('{')
        val end = text.lastIndexOf('}')
        if (start < 0 || end <= start) return null
        return runCatching { JSONObject(text.substring(start, end + 1)) }.getOrNull()
    }

    private fun extractJsonArray(text: String): JSONArray? {
        val start = text.indexOf('[')
        val end = text.lastIndexOf(']')
        if (start < 0 || end <= start) return null
        return runCatching { JSONArray(text.substring(start, end + 1)) }.getOrNull()
    }

    private fun parseMountStats(ms: JSONObject?): MountStats? {
        if (ms == null) return null
        return MountStats(
            totalMounts = ms.optInt("total_mounts", 0),
            successfulMounts = ms.optInt("successful_mounts", 0),
            failedMounts = ms.optInt("failed_mounts", 0),
            tmpfsCreated = ms.optInt("tmpfs_created", 0),
            filesMounted = ms.optInt("files_mounted", 0),
            dirsMounted = ms.optInt("dirs_mounted", 0),
            symlinksCreated = ms.optInt("symlinks_created", 0),
            overlayfsMounts = ms.optInt("overlayfs_mounts", 0),
            successRate = if (ms.has("success_rate")) ms.optDouble("success_rate") else null
        )
    }

    private fun parsePartitions(parts: JSONArray?): List<PartitionInfo> {
        if (parts == null) return emptyList()
        return (0 until parts.length()).mapNotNull { i ->
            val p = parts.optJSONObject(i) ?: return@mapNotNull null
            PartitionInfo(
                name = p.optString("name", ""),
                mountPoint = p.optString("mount_point", ""),
                fsType = p.optString("fs_type", ""),
                isReadOnly = p.optBoolean("is_read_only", false),
                existsAsSymlink = p.optBoolean("exists_as_symlink", false)
            )
        }
    }
    
    // ==================== Commands ====================
    
    /**
     * Get HymoFS version from ksud
     */
    suspend fun getVersion(): String = withContext(Dispatchers.IO) {
        try {
            // hymo: hymo hymofs version -> JSON
            val result = Shell.cmd("${getKsud()} hymo hymofs version").exec()
            if (!result.isSuccess) {
                return@withContext "Unknown"
            }

            val json = JSONObject(result.out.joinToString("\n"))
            val protocolVersion = json.optInt("protocol_version", -1)
            val kernelVersion = json.optInt("kernel_version", -1)
            val hymofsAvailable = json.optBoolean("hymofs_available", false)
            val mismatch = json.optBoolean("protocol_mismatch", false)

            if (!hymofsAvailable) {
                return@withContext "Unavailable"
            }

            // 协议版本和内核版本一致时只显示一个数字，不一致时同时显示并标注 mismatch
            return@withContext if (protocolVersion == kernelVersion && protocolVersion >= 0) {
                protocolVersion.toString()
            } else {
                buildString {
                    append("proto ")
                    append(protocolVersion)
                    append(", kernel ")
                    append(kernelVersion)
                    if (mismatch) {
                        append(" (mismatch)")
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get version", e)
            "Unknown"
        }
    }
    
    /**
     * Read real kernel uname info from sysfs (/proc/sys/kernel/osrelease and version).
     * Same approach as getSystemInfo() kernel field - uses cat to read raw sysfs,
     * NOT uname (which returns spoofed values when HymoFS uname spoofing is active).
     * Returns Pair(unameRelease, unameVersion).
     */
    suspend fun readKernelUnameFromSysfs(): Pair<String, String> = withContext(Dispatchers.IO) {
        val releaseResult = Shell.cmd("cat /proc/sys/kernel/osrelease 2>/dev/null").exec()
        val release = if (releaseResult.isSuccess) releaseResult.out.firstOrNull()?.trim() ?: "" else ""
        val versionResult = Shell.cmd("cat /proc/sys/kernel/version 2>/dev/null").exec()
        val version = if (versionResult.isSuccess) versionResult.out.firstOrNull()?.trim() ?: "" else ""
        Pair(release, version)
    }
    
    /**
     * Get HymoFS status (check if kernel supports it)
     *
     * Uses `ksud hymo config show` JSON output, which includes:
     *   "hymofs_status": int (0=Available, 1=NotPresent, 2=KernelTooOld, 3=ModuleTooOld)
     */
    suspend fun getStatus(): HymoFSStatus = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo config show").exec()
            if (!result.isSuccess) {
                return@withContext HymoFSStatus.NOT_PRESENT
            }
            val json = JSONObject(result.out.joinToString("\n"))
            val code = json.optInt("hymofs_status", HymoFSStatus.NOT_PRESENT.code)
            HymoFSStatus.fromCode(code)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get status", e)
            HymoFSStatus.NOT_PRESENT
        }
    }
    
    /**
     * Load configuration from ksud hymo config show (JSON, hymo format)
     */
    suspend fun loadConfig(): HymoConfig = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo config show").exec()
            if (result.isSuccess) {
                val json = JSONObject(result.out.joinToString("\n"))
                HymoConfig(
                    moduledir = json.optString("moduledir", MODULE_DIR),
                    tempdir = json.optString("tempdir", ""),
                    mountsource = json.optString("mountsource", "KSU"),
                    debug = json.optBoolean("debug", false),
                    verbose = json.optBoolean("verbose", false),
                    fsType = json.optString("fs_type", "auto"),
                    disableUmount = json.optBoolean("disable_umount", false),
                    enableNuke = json.optBoolean("enable_nuke", true),
                    ignoreProtocolMismatch = json.optBoolean("ignore_protocol_mismatch", false),
                    enableKernelDebug = json.optBoolean("enable_kernel_debug", false),
                    enableStealth = json.optBoolean("enable_stealth", true),
                    hymofsEnabled = json.optBoolean("hymofs_enabled", true),
                    mirrorPath = run {
                        val raw = json.optString("mirror_path", "")
                        if (raw == "/data/adb/hymo/img_mnt") "" else raw
                    },
                    partitions = json.optJSONArray("partitions")?.let { arr ->
                        (0 until arr.length()).map { arr.getString(it) }
                    } ?: emptyList(),
                    unameRelease = json.optString("uname_release", ""),
                    unameVersion = json.optString("uname_version", ""),
                    lkmAutoload = json.optBoolean("lkm_autoload", true),
                    lkmKmiOverride = json.optString("lkm_kmi_override", ""),
                    hymofsBuiltin = json.optBoolean("hymofs_builtin", false)
                )
            } else {
                HymoConfig()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load config", e)
            HymoConfig()
        }
    }
    
    /**
     * Save configuration as JSON (config.json, hymo format)
     */
    suspend fun saveConfig(config: HymoConfig): Boolean = withContext(Dispatchers.IO) {
        try {
            val json = JSONObject().apply {
                put("moduledir", config.moduledir)
                if (config.tempdir.isNotEmpty()) put("tempdir", config.tempdir)
                put("mountsource", config.mountsource)
                put("debug", config.debug)
                put("verbose", config.verbose)
                put("fs_type", config.fsType)
                put("disable_umount", config.disableUmount)
                put("enable_nuke", config.enableNuke)
                put("ignore_protocol_mismatch", config.ignoreProtocolMismatch)
                put("enable_kernel_debug", config.enableKernelDebug)
                put("enable_stealth", config.enableStealth)
                put("hymofs_enabled", config.hymofsEnabled)
                if (config.mirrorPath.isNotEmpty()) {
                    put("mirror_path", config.mirrorPath)
                }
                if (config.partitions.isNotEmpty()) {
                    put("partitions", JSONArray(config.partitions))
                }
                if (config.unameRelease.isNotEmpty()) put("uname_release", config.unameRelease)
                if (config.unameVersion.isNotEmpty()) put("uname_version", config.unameVersion)
            }
            val content = json.toString(2)

            val result = Shell.cmd(
                "mkdir -p '$HYMO_CONFIG_DIR'",
                "cat > '$HYMO_CONFIG_FILE' << 'HYMO_CONFIG_EOF'",
                content,
                "HYMO_CONFIG_EOF"
            ).exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to save config", e)
            false
        }
    }
    
    /**
     * Get module list with their mount modes
     */
    suspend fun getModules(): List<ModuleInfo> = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo module list").exec()
            if (result.isSuccess) {
                val output = (result.out + result.err).joinToString("\n")
                val json = JSONObject(output)
                val modulesArray = json.optJSONArray("modules") ?: return@withContext emptyList()
                
                (0 until modulesArray.length()).map { i ->
                    val m = modulesArray.getJSONObject(i)
                    val rulesArray = m.optJSONArray("rules")
                    val rules = if (rulesArray != null) {
                        (0 until rulesArray.length()).map { idx ->
                            val r = rulesArray.getJSONObject(idx)
                            ModuleRule(
                                path = r.optString("path", ""),
                                mode = r.optString("mode", "auto")
                            )
                        }.filter { it.path.isNotEmpty() }
                    } else {
                        emptyList()
                    }
                    ModuleInfo(
                        id = m.getString("id"),
                        name = m.optString("name", m.getString("id")),
                        version = m.optString("version", ""),
                        author = m.optString("author", ""),
                        description = m.optString("description", ""),
                        mode = m.optString("mode", "auto"),
                        strategy = m.optString("strategy", "overlay"),
                        path = m.optString("path", ""),
                        enabled = !m.optBoolean("disabled", false),
                        rules = rules
                    )
                }
            } else {
                emptyList()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get modules", e)
            emptyList()
        }
    }
    
    /**
     * Get active rules from kernel
     */
    suspend fun getActiveRules(): List<ActiveRule> = withContext(Dispatchers.IO) {
        try {
            // hymo: hymo hymofs list -> JSON array of rules
            val result = Shell.cmd("${getKsud()} hymo hymofs list").exec()
            if (result.isSuccess) {
                val arr = JSONArray(result.out.joinToString("\n"))
                val rules = mutableListOf<ActiveRule>()
                for (i in 0 until arr.length()) {
                    val obj = arr.getJSONObject(i)
                    val typeUpper = obj.optString("type", "").uppercase()
                    when (typeUpper) {
                        "ADD", "MERGE" -> {
                            val target = obj.optString("target", "")
                            val source = obj.optString("source", "")
                            rules.add(ActiveRule(typeUpper.lowercase(), source, target, null))
                        }
                        "HIDE" -> {
                            val path = obj.optString("path", "")
                            rules.add(ActiveRule("hide", path, null, null))
                        }
                        else -> {
                            val args = obj.optString("args", "")
                            if (args.isNotEmpty()) {
                                rules.add(ActiveRule(typeUpper.lowercase(), args, null, null))
                            }
                        }
                    }
                }
                rules
            } else {
                emptyList()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get active rules", e)
            emptyList()
        }
    }

    /**
     * List user-defined hide rules (from user_hide_rules.json via hymo hide list).
     */
    suspend fun listUserHideRules(): List<String> = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo hide list").exec()
            if (result.isSuccess) {
                val json = JSONArray(result.out.joinToString("\n"))
                (0 until json.length()).map { idx -> json.getString(idx) }
            } else {
                emptyList()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to list user hide rules", e)
            emptyList()
        }
    }

    /**
     * Add a user hide rule (absolute path) and apply to kernel when possible.
     */
    suspend fun addUserHideRule(path: String): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo hide add '$path'").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to add user hide rule", e)
            false
        }
    }

    /**
     * Remove a user hide rule by path.
     */
    suspend fun removeUserHideRule(path: String): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo hide remove '$path'").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to remove user hide rule", e)
            false
        }
    }
    
    /**
     * Get storage usage
     */
    suspend fun getStorageInfo(): StorageInfo = withContext(Dispatchers.IO) {
        try {
            // hymo: hymo api storage -> JSON
            val result = Shell.cmd("${getKsud()} hymo api storage").exec()
            if (result.isSuccess) {
                val json = JSONObject(result.out.joinToString("\n"))
                val percentValue = json.optDouble("percent", 0.0)
                StorageInfo(
                    size = json.optString("size", "-"),
                    used = json.optString("used", "-"),
                    avail = json.optString("avail", "-"),
                    percent = "${percentValue.toInt()}%",
                    type = json.optString("mode", "unknown")
                )
            } else {
                StorageInfo("-", "-", "-", "0%", "unknown")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get storage info", e)
            StorageInfo("-", "-", "-", "0%", "unknown")
        }
    }
    
    /**
     * Get system info including daemon state (aligned with WebUI: api system + hymofs version)
     */
    suspend fun getSystemInfo(): SystemInfo = withContext(Dispatchers.IO) {
        try {
            // Get kernel from /proc/sys (real kernel, not spoofed by uname)
            val kernelResult = Shell.cmd("cat /proc/sys/kernel/osrelease 2>/dev/null").exec()
            val kernel = if (kernelResult.isSuccess) kernelResult.out.firstOrNull()?.trim() ?: "Unknown" else "Unknown"
            
            val selinuxResult = Shell.cmd("getenforce").exec()
            val selinux = if (selinuxResult.isSuccess) selinuxResult.out.firstOrNull() ?: "Unknown" else "Unknown"
            
            var mountBase = "Unknown"
            var activeMounts = emptyList<String>()
            var hymofsModuleIds = emptyList<String>()
            var hymofsMismatch = false
            var mismatchMessage: String? = null
            var mountStats: MountStats? = null
            var detectedPartitions = emptyList<PartitionInfo>()
            
            // Call hymo api system for mountStats, detectedPartitions, mount_base
            val apiSystemResult = Shell.cmd("${getKsud()} hymo api system").exec()
            if (apiSystemResult.isSuccess && apiSystemResult.out.isNotEmpty()) {
                try {
                    val sysText = (apiSystemResult.out + apiSystemResult.err).joinToString("\n")
                    val sys = extractJsonObject(sysText) ?: throw IllegalStateException("api system json parse failed")
                    mountBase = sys.optString("mount_base", mountBase)
                    mountStats = parseMountStats(sys.optJSONObject("mountStats"))
                    detectedPartitions = parsePartitions(sys.optJSONArray("detectedPartitions"))
                } catch (e: Exception) {
                    Log.w(TAG, "Failed to parse api system", e)
                }
            }

            // WebUI-compatible fallback path: pull dedicated endpoints if system payload is partial.
            if (mountStats == null) {
                val mountStatsResult = Shell.cmd("${getKsud()} hymo api mount-stats").exec()
                if (mountStatsResult.isSuccess) {
                    val msText = (mountStatsResult.out + mountStatsResult.err).joinToString("\n")
                    mountStats = parseMountStats(extractJsonObject(msText))
                }
            }
            if (detectedPartitions.isEmpty()) {
                val partitionsResult = Shell.cmd("${getKsud()} hymo api partitions").exec()
                if (partitionsResult.isSuccess) {
                    val partsText = (partitionsResult.out + partitionsResult.err).joinToString("\n")
                    detectedPartitions = parsePartitions(extractJsonArray(partsText))
                }
            }
            
            // Call hymo hymofs version for active_modules, protocol_mismatch, mismatch_message
            val hymofsVersionResult = Shell.cmd("${getKsud()} hymo hymofs version").exec()
            if (hymofsVersionResult.isSuccess && hymofsVersionResult.out.isNotEmpty()) {
                try {
                    val verText = (hymofsVersionResult.out + hymofsVersionResult.err).joinToString("\n")
                    val ver = extractJsonObject(verText) ?: throw IllegalStateException("hymofs version json parse failed")
                    hymofsModuleIds = ver.optJSONArray("active_modules")?.let { arr ->
                        (0 until arr.length()).map { arr.getString(it) }
                    } ?: hymofsModuleIds
                    hymofsMismatch = ver.optBoolean("protocol_mismatch", hymofsMismatch)
                    val mm = ver.optString("mismatch_message", "")
                    if (mm.isNotBlank()) mismatchMessage = mm
                } catch (e: Exception) {
                    Log.w(TAG, "Failed to parse hymofs version", e)
                }
            }
            
            // Fallback: daemon state file (also used to populate active_mounts for status page).
            run {
                val stateResult = Shell.cmd("cat '$HYMO_STATE_FILE' 2>/dev/null").exec()
                if (stateResult.isSuccess && stateResult.out.isNotEmpty()) {
                    try {
                        val stateText = (stateResult.out + stateResult.err).joinToString("\n")
                        val state = extractJsonObject(stateText) ?: throw IllegalStateException("state json parse failed")
                        if (mountBase == "Unknown") mountBase = state.optString("mount_point", "Unknown")
                        activeMounts = state.optJSONArray("active_mounts")?.let { arr ->
                            (0 until arr.length()).map { arr.getString(it) }
                        } ?: activeMounts
                        if (hymofsModuleIds.isEmpty()) {
                            hymofsModuleIds = state.optJSONArray("hymofs_module_ids")?.let { arr ->
                                (0 until arr.length()).map { arr.getString(it) }
                            } ?: emptyList()
                        }
                        hymofsMismatch = state.optBoolean("hymofs_mismatch", hymofsMismatch)
                        if (mismatchMessage.isNullOrBlank()) {
                            val mm = state.optString("mismatch_message", "")
                            if (mm.isNotBlank()) mismatchMessage = mm
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "Failed to parse daemon state", e)
                    }
                }
            }
            
            SystemInfo(kernel, selinux, mountBase, activeMounts, hymofsModuleIds, hymofsMismatch, mismatchMessage, mountStats, detectedPartitions)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get system info", e)
            SystemInfo("Unknown", "Unknown", "Unknown", emptyList(), emptyList(), false, null)
        }
    }
    
    /**
     * Set module mount mode
     */
    suspend fun setModuleMode(moduleId: String, mode: String): Boolean = withContext(Dispatchers.IO) {
        try {
            // hymo: delegate to CLI: hymo module set-mode <id> <mode>
            val result = Shell.cmd("${getKsud()} hymo module set-mode $moduleId $mode").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set module mode", e)
            false
        }
    }

    /**
     * Add a per-module custom rule: <path> -> <mode>
     *
     * Delegates to: hymo module add-rule <mod_id> <path> <mode>
     */
    suspend fun addModuleRule(moduleId: String, path: String, mode: String): Boolean =
        withContext(Dispatchers.IO) {
            try {
                val safePath = path.trim()
                if (safePath.isEmpty()) return@withContext false
                val result =
                    Shell.cmd("${getKsud()} hymo module add-rule $moduleId '$safePath' $mode").exec()
                result.isSuccess
            } catch (e: Exception) {
                Log.e(TAG, "Failed to add module rule for $moduleId", e)
                false
            }
        }

    /**
     * Remove a per-module custom rule by path.
     *
     * Delegates to: hymo module remove-rule <mod_id> <path>
     */
    suspend fun removeModuleRule(moduleId: String, path: String): Boolean =
        withContext(Dispatchers.IO) {
            try {
                val safePath = path.trim()
                if (safePath.isEmpty()) return@withContext false
                val result =
                    Shell.cmd("${getKsud()} hymo module remove-rule $moduleId '$safePath'").exec()
                result.isSuccess
            } catch (e: Exception) {
                Log.e(TAG, "Failed to remove module rule for $moduleId", e)
                false
            }
        }

    /**
     * Ask hymod to scan partitions and update config.json, then return fresh config.
     */
    suspend fun syncPartitionsWithDaemon(): HymoConfig? = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo config sync-partitions").exec()
            if (!result.isSuccess) {
                Log.w(TAG, "sync-partitions failed: ${result.err.joinToString(";")}")
                return@withContext null
            }
            // Reload config after daemon updated it
            loadConfig()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to sync partitions via daemon", e)
            null
        }
    }

    /**
     * Set kernel debug mode
     */
    suspend fun setKernelDebug(enable: Boolean): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo debug ${if (enable) "enable" else "disable"}").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set kernel debug", e)
            false
        }
    }
    
    /**
     * Set stealth mode (aligned with WebUI: hymo debug stealth enable|disable)
     */
    suspend fun setStealth(enable: Boolean): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo debug stealth ${if (enable) "enable" else "disable"}").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set stealth mode", e)
            false
        }
    }

    /**
     * Apply kernel uname spoofing immediately (hymo debug set-uname).
     * Call after saveConfig for immediate effect.
     */
    suspend fun setUname(release: String, version: String): Boolean = withContext(Dispatchers.IO) {
        try {
            val r = release.replace("'", "'\\''")
            val v = version.replace("'", "'\\''")
            val result = Shell.cmd("${getKsud()} hymo debug set-uname '$r' '$v'").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set uname", e)
            false
        }
    }
    
    /**
     * Set manual KMI override for LKM loading (when auto-detect fails).
     */
    suspend fun setLkmKmiOverride(kmi: String): Boolean = withContext(Dispatchers.IO) {
        try {
            // Escape single quotes for shell: ' -> '\''
            val escaped = kmi.replace("'", "'\\''")
            val result = Shell.cmd("${getKsud()} hymo lkm set-kmi '$escaped'").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set LKM KMI override", e)
            false
        }
    }

    /**
     * Clear manual KMI override.
     */
    suspend fun clearLkmKmiOverride(): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo lkm clear-kmi").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to clear LKM KMI override", e)
            false
        }
    }

    /**
     * Load HymoFS LKM manually.
     */
    suspend fun loadLkm(): Boolean = withContext(Dispatchers.IO) {
        try {
            val marker = "__KSUD_EC__:"
            val uidMarker = "__UID__:"
            val cmd = "${getKsud()} hymo lkm load"
            val result = Shell.cmd("{ echo ${uidMarker}\$(id -u); $cmd 2>&1; ec=$?; echo ${marker}\$ec; exit \$ec; }").exec()
            val exitCode = result.out.firstOrNull { it.startsWith(marker) }
                ?.removePrefix(marker)?.trim()?.toIntOrNull()
            val uid = result.out.firstOrNull { it.startsWith(uidMarker) }
                ?.removePrefix(uidMarker)?.trim()
            if (!result.isSuccess) {
                Log.e(
                    TAG,
                    "loadLkm failed: ksud=${getKsud()} uid=$uid exitCode=$exitCode out=${result.out} err=${result.err}"
                )
            }
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load LKM", e)
            false
        }
    }

    /**
     * Unload HymoFS LKM.
     */
    suspend fun unloadLkm(): Boolean = withContext(Dispatchers.IO) {
        try {
            val marker = "__KSUD_EC__:"
            val uidMarker = "__UID__:"
            val cmd = "${getKsud()} hymo lkm unload"
            val result = Shell.cmd("{ echo ${uidMarker}\$(id -u); $cmd 2>&1; ec=$?; echo ${marker}\$ec; exit \$ec; }").exec()
            val exitCode = result.out.firstOrNull { it.startsWith(marker) }
                ?.removePrefix(marker)?.trim()?.toIntOrNull()
            val uid = result.out.firstOrNull { it.startsWith(uidMarker) }
                ?.removePrefix(uidMarker)?.trim()
            if (!result.isSuccess) {
                Log.e(
                    TAG,
                    "unloadLkm failed: ksud=${getKsud()} uid=$uid exitCode=$exitCode out=${result.out} err=${result.err}"
                )
                // Some devices report non-zero on unload path even though module is gone.
                // Verify real state before surfacing failure to UI.
                val statusResult = Shell.cmd("${getKsud()} hymo lkm status").exec()
                if (statusResult.isSuccess) {
                    val statusJson = JSONObject(statusResult.out.joinToString("\n"))
                    if (!statusJson.optBoolean("loaded", true)) {
                        Log.w(TAG, "unloadLkm: command failed but status shows unloaded, treat as success")
                        return@withContext true
                    }
                }
            }
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to unload LKM", e)
            false
        }
    }

    /**
     * Get current LKM hooks (when HymoFS is available).
     */
    suspend fun getLkmHooks(): String = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo api hooks").exec()
            if (result.isSuccess) {
                (result.out + result.err).joinToString("\n").trim()
            } else {
                ""
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get LKM hooks", e)
            ""
        }
    }

    /**
     * Set HymoFS LKM boot autoload (post-fs-data).
     * When enabled: ksud loads LKM at boot; failures are logged to /data/adb/hymo/lkm_autoload.log
     * When disabled: skip loading at boot
     */
    suspend fun setLkmAutoload(enable: Boolean): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo lkm set-autoload ${if (enable) "on" else "off"}").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set LKM autoload", e)
            false
        }
    }
    
    /**
     * Fix mount IDs
     */
    suspend fun fixMounts(): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo fix-mounts").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to fix mounts", e)
            false
        }
    }
    
    /**
     * Clear all rules
     */
    suspend fun clearAllRules(): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo clear").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to clear rules", e)
            false
        }
    }
    
    /**
     * Read daemon log
     */
    suspend fun readLog(lines: Int = 500): String = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("tail -n $lines '$HYMO_LOG_FILE' 2>/dev/null").exec()
            if (result.isSuccess) {
                result.out.joinToString("\n")
            } else {
                ""
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to read log", e)
            ""
        }
    }
    
    /**
     * Read kernel log (dmesg) for HymoFS
     */
    suspend fun readKernelLog(lines: Int = 200): String = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("dmesg | grep -i 'hymofs\\|hymo' | tail -n $lines").exec()
            if (result.isSuccess) {
                result.out.joinToString("\n")
            } else {
                ""
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to read kernel log", e)
            ""
        }
    }
    
    /**
     * Trigger mount operation (if supported)
     */
    suspend fun triggerMount(): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo mount").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to trigger mount", e)
            false
        }
    }
    
    /**
     * Check if built-in mount is enabled (no disable file exists)
     */
    suspend fun isBuiltinMountEnabled(): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("test -f '$DISABLE_BUILTIN_MOUNT_FILE' && echo 'disabled' || echo 'enabled'").exec()
            result.isSuccess && result.out.firstOrNull()?.trim() == "enabled"
        } catch (e: Exception) {
            Log.e(TAG, "Failed to check builtin mount status", e)
            true // Default to enabled
        }
    }
    
    /**
     * Set built-in mount state
     * @param enable true to enable built-in mount (remove disable file), false to disable (create disable file)
     */
    suspend fun setBuiltinMountEnabled(enable: Boolean): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = if (enable) {
                Shell.cmd("rm -f '$DISABLE_BUILTIN_MOUNT_FILE'").exec()
            } else {
                Shell.cmd("touch '$DISABLE_BUILTIN_MOUNT_FILE'").exec()
            }
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set builtin mount state", e)
            false
        }
    }
    
    /**
     * Scan for partition candidates in module directories
     * Returns list of detected partition names that are mountpoints
     */
    suspend fun scanPartitionCandidates(moduleDir: String = MODULE_DIR): List<String> = withContext(Dispatchers.IO) {
        try {
            // Built-in standard partitions to ignore
            val ignored = setOf(
                "META-INF", "common", "system", "vendor", "product", "system_ext",
                "odm", "oem", ".git", ".github", "lost+found"
            )
            
            val candidates = mutableSetOf<String>()
            val moduleDirFile = File(moduleDir)
            
            if (!moduleDirFile.exists() || !moduleDirFile.isDirectory) {
                return@withContext emptyList()
            }
            
            // Scan each module directory
            moduleDirFile.listFiles()?.forEach { moduleFile ->
                if (!moduleFile.isDirectory) return@forEach
                
                // Check subdirectories in each module
                moduleFile.listFiles()?.forEach { subdir ->
                    if (!subdir.isDirectory) return@forEach
                    
                    val name = subdir.name
                    if (ignored.contains(name)) return@forEach
                    
                    // Check if it corresponds to a real mountpoint in root
                    val rootPath = "/$name"
                    val checkResult = Shell.cmd(
                        "test -d '$rootPath' && mountpoint -q '$rootPath' && echo 'yes' || echo 'no'"
                    ).exec()
                    
                    if (checkResult.isSuccess && checkResult.out.firstOrNull()?.trim() == "yes") {
                        candidates.add(name)
                    }
                }
            }
            
            candidates.sorted()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to scan partition candidates", e)
            emptyList()
        }
    }
}
