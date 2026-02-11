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
    data class ModuleInfo(
        val id: String,
        val name: String,
        val version: String,
        val author: String,
        val description: String,
        val mode: String,    // auto, hymofs, overlay, magic, none
        val strategy: String, // resolved strategy: hymofs, overlay, magic
        val path: String,
        val enabled: Boolean = true
    )
    
    /**
     * System info data class
     */
    data class SystemInfo(
        val kernel: String,
        val selinux: String,
        val mountBase: String,
        val activeMounts: List<String>,
        val hymofsModuleIds: List<String>,
        val hymofsMismatch: Boolean,
        val mismatchMessage: String?
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
     * Config data class (aligned with meta-hymo Config)
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
        val mountStage: String = "metamount",     // "post-fs-data", "metamount", "services"
        val partitions: List<String> = emptyList(),
        val unameRelease: String = "",
        val unameVersion: String = ""
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
    
    // ==================== Commands ====================
    
    /**
     * Get HymoFS version from ksud
     */
    suspend fun getVersion(): String = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo version").exec()
            if (result.isSuccess) {
                result.out.joinToString("\n").trim()
            } else {
                "Unknown"
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get version", e)
            "Unknown"
        }
    }
    
    /**
     * Get HymoFS status (check if kernel supports it)
     */
    suspend fun getStatus(): HymoFSStatus = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo status").exec()
            if (result.isSuccess) {
                val output = result.out.joinToString("\n")
                when {
                    output.contains("Available") -> HymoFSStatus.AVAILABLE
                    output.contains("Not Present") || output.contains("NotPresent") -> HymoFSStatus.NOT_PRESENT
                    output.contains("Kernel Too Old") || output.contains("KernelTooOld") -> HymoFSStatus.KERNEL_TOO_OLD
                    output.contains("Module Too Old") || output.contains("ModuleTooOld") -> HymoFSStatus.MODULE_TOO_OLD
                    else -> HymoFSStatus.NOT_PRESENT
                }
            } else {
                HymoFSStatus.NOT_PRESENT
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get status", e)
            HymoFSStatus.NOT_PRESENT
        }
    }
    
    /**
     * Load configuration from ksud hymo config show (JSON, meta-hymo format)
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
                    mirrorPath = json.optString("mirror_path", ""),
                    mountStage = json.optString("mount_stage", "metamount"),
                    partitions = json.optJSONArray("partitions")?.let { arr ->
                        (0 until arr.length()).map { arr.getString(it) }
                    } ?: emptyList(),
                    unameRelease = json.optString("uname_release", ""),
                    unameVersion = json.optString("uname_version", "")
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
     * Save configuration as JSON (config.json, meta-hymo format)
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
                if (config.mountStage.isNotEmpty()) {
                    put("mount_stage", config.mountStage)
                }
                if (config.partitions.isNotEmpty()) {
                    put("partitions", JSONArray(config.partitions))
                }
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
            val result = Shell.cmd("${getKsud()} hymo modules").exec()
            if (result.isSuccess) {
                val json = JSONObject(result.out.joinToString("\n"))
                val modulesArray = json.optJSONArray("modules") ?: return@withContext emptyList()
                
                (0 until modulesArray.length()).map { i ->
                    val m = modulesArray.getJSONObject(i)
                    ModuleInfo(
                        id = m.getString("id"),
                        name = m.optString("name", m.getString("id")),
                        version = m.optString("version", ""),
                        author = m.optString("author", ""),
                        description = m.optString("description", ""),
                        mode = m.optString("mode", "auto"),
                        strategy = m.optString("strategy", "overlay"),
                        path = m.optString("path", ""),
                        enabled = !m.optBoolean("disabled", false)
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
            val result = Shell.cmd("${getKsud()} hymo list").exec()
            if (result.isSuccess) {
                val rules = mutableListOf<ActiveRule>()
                result.out.forEach { line ->
                    if (line.startsWith("add ")) {
                        val parts = line.substring(4).split(" ")
                        if (parts.size >= 3) {
                            rules.add(ActiveRule("add", parts[0], parts[1], parts[2].toIntOrNull()))
                        }
                    } else if (line.startsWith("hide ")) {
                        rules.add(ActiveRule("hide", line.substring(5)))
                    } else if (line.startsWith("inject ")) {
                        rules.add(ActiveRule("inject", line.substring(7)))
                    } else if (line.startsWith("merge ")) {
                        val parts = line.substring(6).split(" ")
                        if (parts.size >= 2) {
                            rules.add(ActiveRule("merge", parts[0], parts[1]))
                        }
                    } else if (line.startsWith("hide_xattr_sb ")) {
                        rules.add(ActiveRule("hide_xattr_sb", line.substring(14)))
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
     * Get storage usage
     */
    suspend fun getStorageInfo(): StorageInfo = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo storage").exec()
            if (result.isSuccess) {
                val json = JSONObject(result.out.joinToString("\n"))
                StorageInfo(
                    size = json.optString("size", "-"),
                    used = json.optString("used", "-"),
                    avail = json.optString("avail", "-"),
                    percent = json.optString("percent", "0%"),
                    type = json.optString("type", "unknown")
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
     * Get system info including daemon state
     */
    suspend fun getSystemInfo(): SystemInfo = withContext(Dispatchers.IO) {
        try {
            // Get kernel and selinux
            val kernelResult = Shell.cmd("uname -r").exec()
            val kernel = if (kernelResult.isSuccess) kernelResult.out.firstOrNull() ?: "Unknown" else "Unknown"
            
            val selinuxResult = Shell.cmd("getenforce").exec()
            val selinux = if (selinuxResult.isSuccess) selinuxResult.out.firstOrNull() ?: "Unknown" else "Unknown"
            
            // Get daemon state
            val stateResult = Shell.cmd("cat '$HYMO_STATE_FILE' 2>/dev/null").exec()
            var mountBase = "Unknown"
            var activeMounts = emptyList<String>()
            var hymofsModuleIds = emptyList<String>()
            var hymofsMismatch = false
            var mismatchMessage: String? = null
            
            if (stateResult.isSuccess && stateResult.out.isNotEmpty()) {
                try {
                    val state = JSONObject(stateResult.out.joinToString("\n"))
                    mountBase = state.optString("mount_point", "Unknown")
                    activeMounts = state.optJSONArray("active_mounts")?.let { arr ->
                        (0 until arr.length()).map { arr.getString(it) }
                    } ?: emptyList()
                    hymofsModuleIds = state.optJSONArray("hymofs_module_ids")?.let { arr ->
                        (0 until arr.length()).map { arr.getString(it) }
                    } ?: emptyList()
                    hymofsMismatch = state.optBoolean("hymofs_mismatch", false)
                    mismatchMessage = state.optString("mismatch_message", null)
                } catch (e: Exception) {
                    Log.w(TAG, "Failed to parse daemon state", e)
                }
            }
            
            SystemInfo(kernel, selinux, mountBase, activeMounts, hymofsModuleIds, hymofsMismatch, mismatchMessage)
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
            // Read current modes file
            val modesFile = "$HYMO_CONFIG_DIR/module_mode.conf"
            val currentModes = mutableMapOf<String, String>()
            
            val readResult = Shell.cmd("cat '$modesFile' 2>/dev/null").exec()
            if (readResult.isSuccess) {
                readResult.out.forEach { line ->
                    if (line.isNotBlank() && !line.startsWith("#") && line.contains("=")) {
                        val parts = line.split("=", limit = 2)
                        if (parts.size == 2) {
                            currentModes[parts[0].trim()] = parts[1].trim()
                        }
                    }
                }
            }
            
            // Update mode
            if (mode == "auto") {
                currentModes.remove(moduleId)
            } else {
                currentModes[moduleId] = mode
            }
            
            // Write back
            val content = buildString {
                appendLine("# Module Modes")
                currentModes.forEach { (id, m) ->
                    appendLine("$id=$m")
                }
            }
            
            val escapedContent = content.replace("'", "'\\''")
            val result = Shell.cmd(
                "mkdir -p '$HYMO_CONFIG_DIR'",
                "printf '%s\\n' '$escapedContent' > '$modesFile'"
            ).exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set module mode", e)
            false
        }
    }
    
    /**
     * Set kernel debug mode
     */
    suspend fun setKernelDebug(enable: Boolean): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo debug ${if (enable) "on" else "off"}").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set kernel debug", e)
            false
        }
    }
    
    /**
     * Set stealth mode
     */
    suspend fun setStealth(enable: Boolean): Boolean = withContext(Dispatchers.IO) {
        try {
            val result = Shell.cmd("${getKsud()} hymo stealth ${if (enable) "on" else "off"}").exec()
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set stealth mode", e)
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
