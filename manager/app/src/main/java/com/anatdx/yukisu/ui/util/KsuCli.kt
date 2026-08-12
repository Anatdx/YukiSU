package com.anatdx.yukisu.ui.util

import android.annotation.SuppressLint
import android.content.ContentResolver
import android.content.Context
import android.database.Cursor
import android.net.Uri
import android.os.Environment
import android.os.Parcelable
import android.os.SystemClock
import android.provider.OpenableColumns
import android.util.Log
import com.anatdx.yukisu.R
import com.topjohnwu.superuser.CallbackList
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.parcelize.Parcelize
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.ksu.KsuPaths
import com.anatdx.yukisu.ksuApp
import com.topjohnwu.superuser.io.SuFile
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import java.util.Properties


/**
 * @author weishu
 * @date 2023/1/1.
 */
private const val TAG = "KsuCli"

private fun getKsuDaemonPath(): String {
    return ksuApp.applicationInfo.nativeLibraryDir + File.separator + "libksud.so"
}

/**
 * Public function to get ksud path for other modules
 */
fun getKsud(): String = getKsuDaemonPath()

object KsuCli {
    var SHELL: Shell = createRootShell()
        private set
    var GLOBAL_MNT_SHELL: Shell = createRootShell(true)
        private set
    
    /**
     * Recreate shell instances after SuperKey authentication.
     * This is necessary because the initial shells were created before
     * the app had root permission.
     * 
     * Also checks and installs ksud if needed (SuperKey mode: manager authenticates first,
     * then we can install ksud with proper permissions).
     */
    fun refreshShells() {
        Log.d(TAG, "refreshShells: starting, old SHELL.isRoot=${SHELL.isRoot}")
        try {
            SHELL.close()
        } catch (_: Exception) {}
        try {
            GLOBAL_MNT_SHELL.close()
        } catch (_: Exception) {}
        
        // Check if we're now a manager before creating shells
        val isManagerNow = try {
            Natives.isManager
        } catch (e: Exception) {
            Log.e(TAG, "refreshShells: failed to check isManager", e)
            false
        }
        Log.d(TAG, "refreshShells: Natives.isManager=$isManagerNow")
        
        SHELL = createRootShell()
        GLOBAL_MNT_SHELL = createRootShell(true)
        Log.d(TAG, "Shells refreshed, SHELL.isRoot=${SHELL.isRoot}, GLOBAL_MNT_SHELL.isRoot=${GLOBAL_MNT_SHELL.isRoot}")
        
        // After authentication, check if ksud needs to be installed/updated
        if (isManagerNow && SHELL.isRoot) {
            checkAndInstallKsud()
        }
    }
    
    /**
     * Check if ksud needs to be installed or updated.
     * Called after SuperKey authentication succeeds.
     */
    private fun checkAndInstallKsud() {
        try {
            val apkKsudVersion = getApkKsudVersion()
            val installedKsudVersion = getInstalledKsudVersion()
            val binaryMatches = installedKsudVersion != null && isInstalledKsudBinaryCurrent()
            
            Log.i(
                TAG,
                "checkAndInstallKsud: apk=$apkKsudVersion, installed=$installedKsudVersion, " +
                    "binaryMatches=$binaryMatches"
            )
            
            // Version-only checks miss local/reproducible builds made from the same
            // commit. Compare the actual ELF so the installed daemon always carries
            // the exact assets and fixes bundled by this APK.
            if (!binaryMatches) {
                Log.i(
                    TAG,
                    "Installing/updating ksud daemon: apk=$apkKsudVersion, " +
                        "installed=$installedKsudVersion, binaryMatches=$binaryMatches"
                )
                installOrUpdateKsudDaemon()
            } else {
                if (apkKsudVersion != installedKsudVersion) {
                    Log.w(
                        TAG,
                        "ksud version metadata differs but the installed binary is byte-identical: " +
                            "apk=$apkKsudVersion, installed=$installedKsudVersion"
                    )
                }
                Log.d(TAG, "ksud is up-to-date: $installedKsudVersion")
                refreshYukiZygiskSnapshotForNextBoot()
            }
        } catch (e: Exception) {
            Log.e(TAG, "checkAndInstallKsud failed, falling back to install", e)
            // Fallback: always try to sync ksud daemon on error
            installOrUpdateKsudDaemon()
        }
    }
    
    /**
     * The APK-bundled ksud version is pinned at build time by
     * manager/build.gradle.kts (`computeKsudBundledVersion`), which mirrors
     * userspace/ksud/scripts/generate_version.py. No need to fork-exec the
     * daemon just to read its version.
     */
    private fun getApkKsudVersion(): String? =
        BuildConfig.KSUD_BUNDLED_VERSION.takeIf { it.isNotBlank() }

    /**
     * Normalize ksud version string to app-style display: vx.x.x-xxxxxxxx.
     * e.g. "1.3.0-1-g56b0efb0" -> "v1.3.0-56b0efb0"
     */
    fun formatKsudVersionForDisplay(raw: String?): String? {
        if (raw.isNullOrBlank()) return null
        val s = raw.trim().removePrefix("v")
        // Match x.x.x optionally followed by -anything; capture semver and trailing alphanumeric for 8-char
        val semverMatch = Regex("""^(\d+\.\d+\.\d+)""").find(s) ?: return "v$s"
        val semver = semverMatch.value
        val rest = s.drop(semver.length).trimStart('-')
        val describeHash = Regex("""(?:^|-)g([a-fA-F0-9]{7,40})""")
            .find(rest)
            ?.groupValues
            ?.get(1)
        val hashPart = describeHash
            ?: Regex("""[a-fA-F0-9]{7,40}""").find(rest)?.value
        val suffix = when {
            hashPart != null -> hashPart.take(8)
            rest.isNotEmpty() -> rest.filter { it.isLetterOrDigit() }.take(8)
            else -> ""
        }
        return if (suffix.isNotEmpty()) "v$semver-$suffix" else "v$semver"
    }

    /**
     * Get installed ksud version from /data/adb/ksud.
     */
    private fun getInstalledKsudVersion(): String? {
        return try {
            val result = ShellUtils.fastCmd(SHELL, "${KsuPaths.KSUD_BIN} version 2>/dev/null")
            if (result.isBlank()) return null
            val match = Regex("""version\s+([^\s]+)""").find(result)
            match?.groupValues?.get(1)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to get installed ksud version", e)
            null
        }
    }

    private fun isInstalledKsudBinaryCurrent(): Boolean {
        if (!SHELL.isRoot) return false
        val apkKsud = File(ksuApp.applicationInfo.nativeLibraryDir, "libksud.so")
        if (!apkKsud.isFile) return false
        return SHELL.newJob()
            .add(
                "cmp -s ${shellQuoteArgument(apkKsud.absolutePath)} " +
                    shellQuoteArgument(KsuPaths.KSUD_BIN)
            )
            .exec()
            .isSuccess
    }

    /**
     * Public helper for UI: get ksud versions (APK-bundled and installed daemon),
     * formatted as vx.x.x-xxxxxxxx. Returns (formattedApk, formattedInstalled).
     */
    suspend fun getKsudVersionsForUi(): Pair<String?, String?> = withContext(Dispatchers.IO) {
        val apk = getApkKsudVersion()
        val installed = getInstalledKsudVersion()
        formatKsudVersionForDisplay(apk) to formatKsudVersionForDisplay(installed)
    }

    /**
     * Public helper for UI: sync ksud daemon binary from APK into /data/adb/ksud.
     */
    suspend fun updateKsudDaemonForUi(): Boolean = withContext(Dispatchers.IO) {
        installOrUpdateKsudDaemon()
        true
    }

    /**
     * Cold-start auto-sync entry point: only runs the version-aware install
     * path when we actually have a root shell. Cheap to call from
     * MainActivity's first LaunchedEffect.
     */
    suspend fun autoSyncKsudIfNeeded(): Unit = withContext(Dispatchers.IO) {
        if (!SHELL.isRoot) {
            Log.d(TAG, "autoSyncKsudIfNeeded: no root shell, skip")
            return@withContext
        }
        checkAndInstallKsud()
    }

    data class DynamicManagerSignature(
        val size: String,
        val hash: String
    )

    fun getDynamicManagerFlagsForUid(uid: Int): Int {
        val items = runCatching { Natives.getDynamicManagers() }.getOrDefault(IntArray(0))
        val appId = uid % 100000
        var index = 0
        while (index + 1 < items.size) {
            if (items[index] == appId) {
                return items[index + 1]
            }
            index += 2
        }
        return 0
    }

    suspend fun getDynamicManagerSignatureForUid(uid: Int): DynamicManagerSignature? =
        withContext(Dispatchers.IO) {
            val stdout = ArrayList<String>()
            val stderr = ArrayList<String>()
            val result = getRootShell().newJob()
                .add(ksudCmd("dynamic get-sign --json --uid $uid"))
                .to(stdout, stderr)
                .exec()
            if (!result.isSuccess) {
                Log.w(TAG, "dynamic get-sign --uid $uid failed: ${stderr.joinToString("\n")}")
                return@withContext null
            }

            runCatching {
                val v2 = JSONObject(stdout.joinToString("\n")).getJSONObject("v2")
                if (!v2.optBoolean("has", false)) {
                    return@runCatching null
                }
                val size = v2.optString("size").takeIf { it.isNotBlank() } ?: return@runCatching null
                val hash = v2.optString("hash").takeIf { it.isNotBlank() } ?: return@runCatching null
                DynamicManagerSignature(size, hash)
            }.getOrElse {
                Log.w(TAG, "Failed to parse dynamic manager signature for uid $uid", it)
                null
            }
        }

    suspend fun setDynamicManagerUid(uid: Int): Boolean = withContext(Dispatchers.IO) {
        val result = getRootShell().newJob()
            .add(ksudCmd("dynamic set-uid $uid"))
            .exec()
        Log.i(TAG, "dynamic set-uid $uid result: ${result.isSuccess}")
        result.isSuccess
    }

    suspend fun deleteDynamicManager(signature: DynamicManagerSignature): Boolean =
        withContext(Dispatchers.IO) {
            val result = getRootShell().newJob()
                .add(ksudCmd("dynamic del ${signature.size} ${signature.hash}"))
                .exec()
            Log.i(TAG, "dynamic del ${signature.size} ${signature.hash} result: ${result.isSuccess}")
            result.isSuccess
        }

    /**
     * Install or update the ksud daemon binary itself, without touching boot image.
     *
     * This mirrors APatch's "安装/升级系统补丁(apd)" flow:
     * we copy the manager-bundled ksud ELF (`libksud.so`) into:
     *   - `/data/adb/ksud` (daemon binary used by kernel/su wrapper)
     *   - `/data/adb/ksu/bin/ksud` (symlink for convenience/tools)
     */
    private fun installOrUpdateKsudDaemon() {
        val shell = getRootShell()
        if (!shell.isRoot) {
            Log.w(TAG, "installOrUpdateKsudDaemon: shell is not root, skip")
            return
        }

        val nativeDir = ksuApp.applicationInfo.nativeLibraryDir
        val ksudSo = File(nativeDir, "libksud.so")
        if (!ksudSo.exists()) {
            Log.e(TAG, "installOrUpdateKsudDaemon: libksud.so not found in $nativeDir")
            return
        }

        val cmds = arrayOf(
            // Ensure directories
            "mkdir -p ${KsuPaths.KSU_BIN_DIR}",
            "mkdir -p ${KsuPaths.KSU_LOG_DIR}",
            // Copy new daemon binary (multi-call tools dispatch via argv0)
            "cp -f ${ksudSo.absolutePath} ${KsuPaths.KSUD_BIN}",
            "chmod 0755 ${KsuPaths.KSUD_BIN}",
            // Tool symlinks all point to the same multi-call binary.
            "ln -sf ${KsuPaths.KSUD_BIN} ${KsuPaths.KSU_BIN_DIR}/ksud",
            "ln -sf ${KsuPaths.KSUD_BIN} ${KsuPaths.KSU_BIN_DIR}/magiskboot",
            "ln -sf ${KsuPaths.KSUD_BIN} ${KsuPaths.KSU_BIN_DIR}/bootctl",
            "ln -sf ${KsuPaths.KSUD_BIN} ${KsuPaths.KSU_BIN_DIR}/resetprop",
            "ln -sf ${KsuPaths.KSUD_BIN} ${KsuPaths.KSU_BIN_DIR}/yzctl",
            // Fix SELinux contexts (ignore errors on non-SEAndroid systems)
            "restorecon ${KsuPaths.KSUD_BIN} || true",
            "restorecon -R ${KsuPaths.KSU_ROOT} || true",
            // Precompute YukiZygisk's early native snapshot before reboot. If the
            // feature is off, ksud clears the snapshot and returns success.
            "${KsuPaths.KSUD_BIN} yzctl refresh-snapshot || true"
        )

        Log.i(TAG, "installOrUpdateKsudDaemon: syncing ${ksudSo.absolutePath} -> /data/adb/ksud")
        val result = shell.newJob().add(*cmds).exec()
        Log.i(TAG, "installOrUpdateKsudDaemon: result code=${result.code}, isSuccess=${result.isSuccess}")
    }

    private fun refreshYukiZygiskSnapshotForNextBoot() {
        val shell = getRootShell()
        if (!shell.isRoot) {
            Log.w(TAG, "refreshYukiZygiskSnapshotForNextBoot: shell is not root, skip")
            return
        }
        val result = shell.newJob()
            .add("${KsuPaths.KSUD_BIN} yzctl refresh-snapshot || true")
            .exec()
        Log.i(
            TAG,
            "refreshYukiZygiskSnapshotForNextBoot: result code=${result.code}, isSuccess=${result.isSuccess}"
        )
    }
}

fun getRootShell(globalMnt: Boolean = false): Shell {
    return if (globalMnt) KsuCli.GLOBAL_MNT_SHELL else {
        KsuCli.SHELL
    }
}

inline fun <T> withNewRootShell(
    globalMnt: Boolean = false,
    block: Shell.() -> T
): T {
    return createRootShell(globalMnt).use(block)
}

fun Uri.getFileName(context: Context): String? {
    var fileName: String? = null
    val contentResolver: ContentResolver = context.contentResolver
    val cursor: Cursor? = contentResolver.query(this, null, null, null, null)
    cursor?.use {
        if (it.moveToFirst()) {
            fileName = it.getString(it.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME))
        }
    }
    return fileName
}

fun createRootShell(globalMnt: Boolean = false): Shell {
    Shell.enableVerboseLogging = BuildConfig.DEBUG
    val builder = Shell.Builder.create()
    return try {
        val shell = if (globalMnt) {
            builder.build(getKsuDaemonPath(), "debug", "su", "-g")
        } else {
            builder.build(getKsuDaemonPath(), "debug", "su")
        }
        Log.d(TAG, "ksud shell created, isRoot=${shell.isRoot}, globalMnt=$globalMnt")
        shell
    } catch (e: Throwable) {
        Log.w(TAG, "ksu failed (globalMnt=$globalMnt): ", e)
        try {
            val shell = if (globalMnt) {
                builder.build("su", "-mm")
            } else {
                builder.build("su")
            }
            Log.d(TAG, "su shell created, isRoot=${shell.isRoot}, globalMnt=$globalMnt")
            shell
        } catch (e: Throwable) {
            Log.e(TAG, "su failed (globalMnt=$globalMnt): ", e)
            val shell = builder.build("sh")
            Log.w(TAG, "fallback to sh, isRoot=${shell.isRoot}")
            shell
        }
    }
}

/** Build a "ksud <args>" command string. Use this instead of pasting
 *  `${getKsuDaemonPath()}` next to a subcommand literal. */
internal fun ksudCmd(args: String): String = "${getKsuDaemonPath()} $args"

fun execKsud(args: String, newShell: Boolean = false): Boolean {
    return if (newShell) {
        withNewRootShell {
            ShellUtils.fastCmdResult(this, ksudCmd(args))
        }
    } else {
        ShellUtils.fastCmdResult(getRootShell(), ksudCmd(args))
    }
}

/** Run a ksud subcommand and return its trimmed stdout (single value). */
internal fun ksudReadString(args: String, shell: Shell = getRootShell()): String =
    ShellUtils.fastCmd(shell, ksudCmd(args)).trim()

/** Run a ksud subcommand and return non-blank trimmed stdout lines. */
internal fun ksudReadLines(args: String, shell: Shell = getRootShell()): List<String> =
    shell.newJob().add(ksudCmd(args)).to(ArrayList(), null).exec().out
        .filter { it.isNotBlank() }.map { it.trim() }

suspend fun getYukiZygiskStatusJson(): String? = withContext(Dispatchers.IO) {
    runCatching { ksudReadString("yzctl status --json") }
        .getOrNull()
        ?.takeIf { it.startsWith('{') && it.endsWith('}') }
}

suspend fun getFeatureStatus(feature: String): String = withContext(Dispatchers.IO) {
    ksudReadLines("feature check $feature")
        .firstOrNull { it == "supported" || it == "unsupported" || it == "managed" }
        .orEmpty()
}

/** Read a feature's current on/off value via `ksud feature get` (parses the
 *  "Status: enabled/disabled" line). Returns false when unsupported. */
suspend fun getFeatureValue(feature: String): Boolean = withContext(Dispatchers.IO) {
    getFeatureValueOrNull(feature, getRootShell()) ?: false
}

internal fun getFeatureValueOrNull(feature: String, shell: Shell): Boolean? {
    val output = ArrayList<String>()
    val result = shell.newJob()
        .add(ksudCmd("feature get $feature"))
        .to(output, null)
        .exec()
    if (!result.isSuccess) return null

    return parseFeatureValue(output)
}

internal fun parseFeatureValue(output: Iterable<String>): Boolean? {
    val enabled = output.any { it.trim().equals("Status: enabled", ignoreCase = true) }
    val disabled = output.any { it.trim().equals("Status: disabled", ignoreCase = true) }
    return when {
        enabled == disabled -> null
        enabled -> true
        else -> false
    }
}

/** Set a feature value and persist it; returns whether ksud reported success. */
suspend fun setFeatureValue(feature: String, enabled: Boolean): Boolean =
    withContext(Dispatchers.IO) {
        execKsud("feature set $feature ${if (enabled) 1 else 0}", true) &&
            execKsud("feature save", true)
    }

const val UTS_FIELD_SYSNAME = 1 shl 0
const val UTS_FIELD_NODENAME = 1 shl 1
const val UTS_FIELD_RELEASE = 1 shl 2
const val UTS_FIELD_VERSION = 1 shl 3
const val UTS_FIELD_MACHINE = 1 shl 4
const val UTS_FIELD_DOMAINNAME = 1 shl 5
private const val UTS_FIELD_VALID_MASK = (1 shl 6) - 1
private const val UTS_MODE_VALID_MASK = (1L shl 2) - 1

data class UtsTemplate(
    val mask: Int = 0,
    val sysname: String = "",
    val nodename: String = "",
    val release: String = "",
    val version: String = "",
    val machine: String = "",
    val domainname: String = "",
)

data class UtsViewStatus(
    val source: String = "none",
    val mode: Long = 0,
    val bootLocked: Boolean = false,
    val originalValid: Boolean = false,
    val lateGaps: Boolean = false,
    val lateCapture: Boolean = false,
    val detachedTaskCount: Int = 0,
    val globalMask: Int = 0,
    val denyMask: Int = 0,
) {
    val globalEnabled: Boolean
        get() = mode and 1L != 0L
    val scopedEnabled: Boolean
        get() = mode and 2L != 0L
}

data class UtsViewReleaseSnapshot(
    val source: String,
    val mode: Long,
    val bootLocked: Boolean,
    val originalRelease: String,
    val effectiveRelease: String,
) {
    val globalEnabled: Boolean
        get() = mode and 1L != 0L
}

suspend fun isUtsViewSupported(): Boolean = withContext(Dispatchers.IO) {
    val result = getRootShell().newJob().add(ksudCmd("uts-view status"))
        .to(ArrayList(), null).exec()
    result.isSuccess && parseUtsViewStatus(result.out) != null
}

private fun parseUtsMask(value: String): Int {
    val normalized = value.trim()
    return if (normalized.startsWith("0x", ignoreCase = true)) {
        normalized.substring(2).toIntOrNull(16)
    } else {
        normalized.toIntOrNull(10)
    } ?: 0
}

private fun parseUtsMaskStrict(value: String?): Int? {
    if (value.isNullOrEmpty()) return null
    val normalized = value.trim()
    val mask = if (normalized.startsWith("0x", ignoreCase = true)) {
        normalized.substring(2).toIntOrNull(16)
    } else {
        normalized.toIntOrNull(10)
    } ?: return null
    return mask.takeIf { it >= 0 && it and UTS_FIELD_VALID_MASK == it }
}

private val utsFields = listOf(
    "sysname" to UTS_FIELD_SYSNAME,
    "nodename" to UTS_FIELD_NODENAME,
    "release" to UTS_FIELD_RELEASE,
    "version" to UTS_FIELD_VERSION,
    "machine" to UTS_FIELD_MACHINE,
    "domainname" to UTS_FIELD_DOMAINNAME,
)

private fun parseUtsTemplate(lines: List<String>, title: String): UtsTemplate? {
    var mask = 0
    var active = false
    var headerFound = false
    val values = mutableMapOf<String, String>()
    for (raw in lines) {
        val header = raw.trimStart()
        val headerTitle = when {
            header.startsWith("global mask:") -> "global"
            header.startsWith("deny mask:") -> "deny"
            header.startsWith("original mask:") -> "original"
            header.startsWith("effective mask:") -> "effective"
            else -> null
        }
        if (headerTitle != null) {
            active = headerTitle == title
            if (active) {
                if (headerFound) return null
                mask = parseUtsMaskStrict(header.substringAfter(':').trim()) ?: return null
                headerFound = true
            }
        } else if (active) {
            // ksud indents fields by exactly two spaces. Remove only that
            // presentation prefix so leading spaces in the UTS value survive.
            if (!raw.startsWith("  ")) return null
            val line = raw.removePrefix("  ")
            val separator = line.indexOf('=')
            if (separator <= 0) return null
            val name = line.substring(0, separator)
            if (utsFields.none { (field, _) -> field == name } ||
                values.put(name, line.substring(separator + 1)) != null
            ) return null
        }
    }
    if (!headerFound || utsFields.any { (name, bit) ->
            (mask and bit != 0) != values.containsKey(name)
        }) {
        return null
    }
    return UtsTemplate(
        mask = mask,
        sysname = values["sysname"].orEmpty(),
        nodename = values["nodename"].orEmpty(),
        release = values["release"].orEmpty(),
        version = values["version"].orEmpty(),
        machine = values["machine"].orEmpty(),
        domainname = values["domainname"].orEmpty(),
    )
}

private fun parseUtsBoolean(value: String?): Boolean? = when (value) {
    "true" -> true
    "false" -> false
    else -> null
}

private val utsViewStatusFields = setOf(
    "source",
    "mode",
    "boot_locked",
    "original_valid",
    "late_gaps",
    "late_capture",
    "detached_task_count",
    "global_mask",
    "deny_mask",
)

private fun parseUtsViewStatus(lines: List<String>): UtsViewStatus? {
    val values = mutableMapOf<String, String>()
    for (line in lines) {
        val separator = line.indexOf('=')
        if (separator <= 0) return null
        val key = line.substring(0, separator)
        val value = line.substring(separator + 1)
        if (key == "note") continue
        if (key !in utsViewStatusFields || values.put(key, value) != null) return null
    }
    if (values.keys != utsViewStatusFields) return null

    val source = values["source"]?.takeIf { it == "none" || it == "boot" || it == "runtime" }
        ?: return null
    val mode = values["mode"]?.toLongOrNull()
        ?.takeIf { it >= 0 && it and UTS_MODE_VALID_MASK == it }
        ?: return null
    val bootLocked = parseUtsBoolean(values["boot_locked"]) ?: return null
    val originalValid = parseUtsBoolean(values["original_valid"]) ?: return null
    if (!originalValid) return null
    val globalEnabled = mode and 1L != 0L
    val stateIsConsistent = when (source) {
        "none" -> !globalEnabled && !bootLocked
        "boot" -> globalEnabled && bootLocked
        "runtime" -> globalEnabled && !bootLocked
        else -> false
    }
    if (!stateIsConsistent) return null

    val lateGaps = parseUtsBoolean(values["late_gaps"]) ?: return null
    val lateCapture = parseUtsBoolean(values["late_capture"]) ?: return null
    val detachedTaskCount = values["detached_task_count"]?.toIntOrNull()
        ?.takeIf { it >= 0 }
        ?: return null
    val globalMask = parseUtsMaskStrict(values["global_mask"]) ?: return null
    val denyMask = parseUtsMaskStrict(values["deny_mask"]) ?: return null
    if ((globalEnabled && globalMask == 0) || (mode and 2L != 0L && denyMask == 0)) {
        return null
    }
    return UtsViewStatus(
        source = source,
        mode = mode,
        bootLocked = bootLocked,
        originalValid = originalValid,
        lateGaps = lateGaps,
        lateCapture = lateCapture,
        detachedTaskCount = detachedTaskCount,
        globalMask = globalMask,
        denyMask = denyMask,
    )
}

private val utsReleaseSnapshotFields = setOf(
    "snapshot_version",
    "abi_version",
    "source",
    "mode",
    "boot_locked",
    "original_valid",
    "original_release_hex",
    "effective_release_hex",
)

private fun decodeUtsReleaseHex(value: String): String? {
    if (value.isEmpty() || value.length > 128 || value.length % 2 != 0) return null
    val bytes = ByteArray(value.length / 2)
    for (index in bytes.indices) {
        val high = value[index * 2].digitToIntOrNull(16) ?: return null
        val low = value[index * 2 + 1].digitToIntOrNull(16) ?: return null
        bytes[index] = ((high shl 4) or low).toByte()
    }
    if (bytes.any { it == 0.toByte() }) return null
    return runCatching {
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes))
            .toString()
    }.getOrNull()?.takeIf { it.isNotEmpty() }
}

private fun parseUtsViewReleaseSnapshot(lines: List<String>): UtsViewReleaseSnapshot? {
    val values = mutableMapOf<String, String>()
    for (line in lines) {
        val separator = line.indexOf('=')
        if (separator <= 0) return null
        val key = line.substring(0, separator)
        val value = line.substring(separator + 1)
        if (key !in utsReleaseSnapshotFields || values.put(key, value) != null) return null
    }
    if (values.keys != utsReleaseSnapshotFields ||
        values["snapshot_version"] != "1" ||
        values["abi_version"] != "2" ||
        values["original_valid"] != "true"
    ) {
        return null
    }

    val source = values["source"]?.takeIf { it == "none" || it == "boot" || it == "runtime" }
        ?: return null
    val mode = values["mode"]?.toLongOrNull()
        ?.takeIf { it >= 0 && it and UTS_MODE_VALID_MASK == it }
        ?: return null
    val bootLocked = parseUtsBoolean(values["boot_locked"]) ?: return null
    val globalEnabled = mode and 1L != 0L
    val stateIsConsistent = when (source) {
        "none" -> !globalEnabled && !bootLocked
        "boot" -> globalEnabled && bootLocked
        "runtime" -> globalEnabled && !bootLocked
        else -> false
    }
    if (!stateIsConsistent) return null

    val originalRelease = decodeUtsReleaseHex(values["original_release_hex"] ?: return null)
        ?: return null
    val effectiveRelease = decodeUtsReleaseHex(values["effective_release_hex"] ?: return null)
        ?: return null
    return UtsViewReleaseSnapshot(
        source = source,
        mode = mode,
        bootLocked = bootLocked,
        originalRelease = originalRelease,
        effectiveRelease = effectiveRelease,
    )
}

private fun readUtsTemplate(subcommand: String, title: String): UtsTemplate? {
    val result = getRootShell().newJob().add(ksudCmd("uts-view $subcommand"))
        .to(ArrayList(), null).exec()
    return if (result.isSuccess) parseUtsTemplate(result.out, title) else null
}

suspend fun getUtsViewStatus(): UtsViewStatus? = withContext(Dispatchers.IO) {
    val result = getRootShell().newJob().add(ksudCmd("uts-view status"))
        .to(ArrayList(), null).exec()
    if (result.isSuccess) parseUtsViewStatus(result.out) else null
}

suspend fun getUtsViewReleaseSnapshot(): UtsViewReleaseSnapshot? = withContext(Dispatchers.IO) {
    val result = getRootShell().newJob().add(ksudCmd("uts-view release-snapshot"))
        .to(ArrayList(), null).exec()
    if (result.isSuccess) parseUtsViewReleaseSnapshot(result.out) else null
}

suspend fun getUtsViewConfig(): Pair<UtsTemplate, UtsTemplate>? = withContext(Dispatchers.IO) {
    val result = getRootShell().newJob().add(ksudCmd("uts-view get"))
        .to(ArrayList(), null).exec()
    if (!result.isSuccess) {
        null
    } else {
        val global = parseUtsTemplate(result.out, "global")
        val deny = parseUtsTemplate(result.out, "deny")
        if (global != null && deny != null) global to deny else null
    }
}

suspend fun getUtsViewOriginal(): UtsTemplate? = withContext(Dispatchers.IO) {
    readUtsTemplate("original", "original")
}

suspend fun getUtsViewEffective(): UtsTemplate? = withContext(Dispatchers.IO) {
    readUtsTemplate("effective", "effective")
}

private fun UtsTemplate.valueFor(bit: Int): String = when (bit) {
    UTS_FIELD_SYSNAME -> sysname
    UTS_FIELD_NODENAME -> nodename
    UTS_FIELD_RELEASE -> release
    UTS_FIELD_VERSION -> version
    UTS_FIELD_MACHINE -> machine
    UTS_FIELD_DOMAINNAME -> domainname
    else -> ""
}

fun UtsTemplate.normalizedForCommit(): UtsTemplate {
    var normalizedMask = mask
    utsFields.forEach { (_, bit) ->
        if (normalizedMask and bit != 0 && valueFor(bit).isEmpty()) {
            normalizedMask = normalizedMask and bit.inv()
        }
    }
    return if (normalizedMask == mask) this else copy(mask = normalizedMask)
}

private const val UTS_BOOT_PATCH_STATE_PREFERENCES = "uts_boot_patch_state"
private const val UTS_BOOT_PATCHED_TOKEN_KEY = "patched_configuration_token"
private const val UTS_BOOT_DRAFT_MASK_KEY = "draft_mask"
private const val UTS_BOOT_DRAFT_SYSNAME_KEY = "draft_sysname"
private const val UTS_BOOT_DRAFT_NODENAME_KEY = "draft_nodename"
private const val UTS_BOOT_DRAFT_RELEASE_KEY = "draft_release"
private const val UTS_BOOT_DRAFT_VERSION_KEY = "draft_version"
private const val UTS_BOOT_DRAFT_MACHINE_KEY = "draft_machine"
private const val UTS_BOOT_DRAFT_DOMAINNAME_KEY = "draft_domainname"
private const val UTS_BOOT_BASELINE_ERROR_PREFIX = "baseline-error:"
private val utsBootTokenPattern = Regex("^[0-9a-f]{64}$")

private fun ByteArray.toHexString(): String {
    val digits = "0123456789abcdef"
    return buildString(size * 2) {
        this@toHexString.forEach { byte ->
            val value = byte.toInt() and 0xff
            append(digits[value ushr 4])
            append(digits[value and 0x0f])
        }
    }
}

fun utsBootConfigurationToken(enabled: Boolean, template: UtsTemplate): String {
    val digest = MessageDigest.getInstance("SHA-256")
    digest.update(if (enabled) 1.toByte() else 0.toByte())
    val normalized = template.normalizedForCommit()
    digest.update(ByteBuffer.allocate(Int.SIZE_BYTES).putInt(normalized.mask).array())
    utsFields.forEach { (_, bit) ->
        if (normalized.mask and bit != 0) {
            val value = normalized.valueFor(bit).toByteArray(Charsets.UTF_8)
            digest.update(ByteBuffer.allocate(Int.SIZE_BYTES).putInt(bit).array())
            digest.update(ByteBuffer.allocate(Int.SIZE_BYTES).putInt(value.size).array())
            digest.update(value)
        }
    }
    return digest.digest().toHexString()
}

private fun utsBootPatchStatePreferences() = ksuApp.getSharedPreferences(
    UTS_BOOT_PATCH_STATE_PREFERENCES,
    Context.MODE_PRIVATE,
)

fun getLastPatchedUtsBootConfigurationToken(): String? =
    utsBootPatchStatePreferences()
        .getString(UTS_BOOT_PATCHED_TOKEN_KEY, null)
        ?.takeIf(utsBootTokenPattern::matches)

fun getOrInitializePatchedUtsBootConfigurationToken(
    currentToken: String,
): String {
    val preferences = utsBootPatchStatePreferences()
    val existing = preferences.getString(UTS_BOOT_PATCHED_TOKEN_KEY, null)
    if (existing == null) {
        val initialized = preferences.edit()
            .putString(UTS_BOOT_PATCHED_TOKEN_KEY, currentToken)
            .commit()
        return if (initialized) {
            preferences.getString(UTS_BOOT_PATCHED_TOKEN_KEY, null)
                ?: UTS_BOOT_BASELINE_ERROR_PREFIX + "read-failed"
        } else {
            UTS_BOOT_BASELINE_ERROR_PREFIX + "write-failed"
        }
    }
    return existing
}

// KTX edit returns Unit, but this commit result is required to keep a failed
// durability write from being treated as a patched baseline.
@SuppressLint("UseKtx")
private fun recordPatchedUtsBootConfigurationToken(token: String): Boolean =
    utsBootPatchStatePreferences()
        .edit()
        .putString(UTS_BOOT_PATCHED_TOKEN_KEY, token)
        .commit()

fun getSavedUtsBootDraft(): UtsTemplate? = runCatching {
    val preferences = utsBootPatchStatePreferences()
    if (!preferences.contains(UTS_BOOT_DRAFT_MASK_KEY)) {
        return@runCatching null
    }
    UtsTemplate(
        mask = preferences.getInt(UTS_BOOT_DRAFT_MASK_KEY, 0),
        sysname = preferences.getString(UTS_BOOT_DRAFT_SYSNAME_KEY, "").orEmpty(),
        nodename = preferences.getString(UTS_BOOT_DRAFT_NODENAME_KEY, "").orEmpty(),
        release = preferences.getString(UTS_BOOT_DRAFT_RELEASE_KEY, "").orEmpty(),
        version = preferences.getString(UTS_BOOT_DRAFT_VERSION_KEY, "").orEmpty(),
        machine = preferences.getString(UTS_BOOT_DRAFT_MACHINE_KEY, "").orEmpty(),
        domainname = preferences.getString(UTS_BOOT_DRAFT_DOMAINNAME_KEY, "").orEmpty(),
    ).normalizedForCommit().also { template ->
        require(template.mask and UTS_FIELD_VALID_MASK == template.mask)
        require(utsFields.all { (_, bit) ->
            val value = template.valueFor(bit)
            template.mask and bit == 0 ||
                (value.toByteArray(Charsets.UTF_8).size <= 64 &&
                    !value.contains('\n') &&
                    !value.contains('\r') &&
                    !value.contains('\u0000'))
        })
    }
}.getOrNull()

// The caller immediately stages the matching boot config or leaves the screen,
// so this write must be durable before the function reports success.
@SuppressLint("UseKtx")
fun saveUtsBootDraft(template: UtsTemplate): Boolean = runCatching {
    val normalized = template.normalizedForCommit()
    require(normalized.mask and UTS_FIELD_VALID_MASK == normalized.mask)
    require(utsFields.all { (_, bit) ->
        val value = normalized.valueFor(bit)
        normalized.mask and bit == 0 ||
            (value.toByteArray(Charsets.UTF_8).size <= 64 &&
                !value.contains('\n') &&
                !value.contains('\r') &&
                !value.contains('\u0000'))
    })
    utsBootPatchStatePreferences()
        .edit()
        .putInt(UTS_BOOT_DRAFT_MASK_KEY, normalized.mask)
        .putString(UTS_BOOT_DRAFT_SYSNAME_KEY, normalized.sysname)
        .putString(UTS_BOOT_DRAFT_NODENAME_KEY, normalized.nodename)
        .putString(UTS_BOOT_DRAFT_RELEASE_KEY, normalized.release)
        .putString(UTS_BOOT_DRAFT_VERSION_KEY, normalized.version)
        .putString(UTS_BOOT_DRAFT_MACHINE_KEY, normalized.machine)
        .putString(UTS_BOOT_DRAFT_DOMAINNAME_KEY, normalized.domainname)
        .commit()
}.getOrDefault(false)

suspend fun setUtsViewTemplate(global: Boolean, template: UtsTemplate): Boolean =
    withContext(Dispatchers.IO) {
        val normalized = template.normalizedForCommit()
        if (normalized.mask and UTS_FIELD_VALID_MASK != normalized.mask) {
            return@withContext false
        }
        if (utsFields.any { (_, bit) ->
                normalized.mask and bit != 0 &&
                    (normalized.valueFor(bit).toByteArray(Charsets.UTF_8).size > 64 ||
                        normalized.valueFor(bit).contains('\n') ||
                        normalized.valueFor(bit).contains('\r') ||
                        normalized.valueFor(bit).contains('\u0000'))
            }) {
            return@withContext false
        }
        val command = buildString {
            append("uts-view ")
            append(if (global) "set-global" else "set-deny")
            utsFields.forEach { (name, bit) ->
                if (normalized.mask and bit != 0) {
                    append(" --")
                    append(name)
                    append(' ')
                    append(shellQuoteArgument(normalized.valueFor(bit)))
                } else {
                    append(" --inherit ")
                    append(name)
                }
            }
        }
        execKsud(command, true)
    }

suspend fun setUtsViewMode(global: Boolean, enabled: Boolean): Boolean =
    withContext(Dispatchers.IO) {
        val command = when {
            global && enabled -> "uts-view enable-global"
            global -> "uts-view disable-global"
            enabled -> "uts-view enable-scoped"
            else -> "uts-view disable-scoped"
        }
        execKsud(command, true)
    }

private fun utsBootConfigFile(): File = File(ksuApp.filesDir, "uts_boot.conf")

fun hasPendingUtsBootConfigFile(): Boolean = utsBootConfigFile().isFile

fun getPendingUtsBootTemplate(): UtsTemplate? = runCatching {
    val file = utsBootConfigFile()
    if (!file.isFile)
        return@runCatching null

    var formatVersion: String? = null
    var haveMask = false
    val values = mutableMapOf<String, String>()
    for (line in file.readLines()) {
        if (line.isBlank() || line.trimStart().startsWith("#")) continue
        val separator = line.indexOf('=')
        if (separator <= 0) return@runCatching null
        val key = line.substring(0, separator).trim()
        val value = line.substring(separator + 1)
        if (key == "format_version") {
            if (formatVersion != null) return@runCatching null
            formatVersion = value.trim()
            continue
        }
        if (key != "mask" && utsFields.none { (name, _) -> name == key }) {
            return@runCatching null
        }
        if (key in values) return@runCatching null
        values[key] = value
        if (key == "mask") haveMask = true
    }
    val rawTemplate = UtsTemplate(
        mask = parseUtsMask(values["mask"].orEmpty()),
        sysname = values["sysname"].orEmpty(),
        nodename = values["nodename"].orEmpty(),
        release = values["release"].orEmpty(),
        version = values["version"].orEmpty(),
        machine = values["machine"].orEmpty(),
        domainname = values["domainname"].orEmpty(),
    )
    if (formatVersion != "1" || rawTemplate.mask == 0 ||
        rawTemplate.mask and UTS_FIELD_VALID_MASK != rawTemplate.mask ||
        utsFields.any { (name, bit) ->
            val selected = rawTemplate.mask and bit != 0
            selected != (name in values) ||
                (selected &&
                    (rawTemplate.valueFor(bit).toByteArray(Charsets.UTF_8).size > 64 ||
                        rawTemplate.valueFor(bit).contains('\r') ||
                        rawTemplate.valueFor(bit).contains('\u0000')))
        }
    ) {
        null
    } else {
        val normalized = rawTemplate.normalizedForCommit()
        if (normalized.mask == 0) {
            file.delete()
            null
        } else {
            normalized
        }
    }
}.getOrNull()

fun savePendingUtsBootTemplate(template: UtsTemplate?): Boolean = runCatching {
    val file = utsBootConfigFile()
    val normalized = template?.normalizedForCommit()?.takeIf { it.mask != 0 }
    if (normalized == null) {
        !file.exists() || file.delete()
    } else {
        require(normalized.mask and UTS_FIELD_VALID_MASK == normalized.mask)
        require(utsFields.all { (_, bit) ->
            normalized.mask and bit == 0 ||
                (normalized.valueFor(bit).toByteArray(Charsets.UTF_8).size <= 64 &&
                    !normalized.valueFor(bit).contains('\n') &&
                    !normalized.valueFor(bit).contains('\r') &&
                    !normalized.valueFor(bit).contains('\u0000'))
        })
        file.parentFile?.mkdirs()
        file.writeText(buildString {
            appendLine("format_version=1")
            appendLine("mask=0x${normalized.mask.toString(16)}")
            utsFields.forEach { (name, bit) ->
                if (normalized.mask and bit != 0) {
                    append(name)
                    append('=')
                    appendLine(normalized.valueFor(bit))
                }
            }
        })
        true
    }
}.getOrDefault(false)

fun getUtsViewOriginalReleaseForLog(): String? =
    readUtsTemplate("original", "original")?.release

fun install() {
    val start = SystemClock.elapsedRealtime()
    val ksudPath = getKsuDaemonPath()
    val libadbrootPath =
        ksuApp.applicationInfo.nativeLibraryDir + File.separator + "libadbroot.so"
    // magiskboot is built into ksud (multi-call binary); pass ksud path so it can exec itself as magiskboot
    Log.i(TAG, "install: ksud=$ksudPath")
    val result = execKsud("install --magiskboot $ksudPath --libadbroot $libadbrootPath", true)
    Log.w(TAG, "install result: $result, cost: ${SystemClock.elapsedRealtime() - start}ms")
}

fun hasMetaModule(): Boolean {
    return getMetaModuleImplement() != "None"
}

fun listModules(): String =
    ksudReadLines("module list").joinToString("\n").ifBlank { "[]" }

fun getModuleCount(): Int {
    val result = listModules()
    runCatching {
        val array = JSONArray(result)
        return array.length()
    }.getOrElse { return 0 }
}

fun getSuperuserCount(): Int {
    return Natives.getSuperuserCount()
}

fun toggleModule(id: String, enable: Boolean): Boolean {
    val cmd = if (enable) {
        "module enable $id"
    } else {
        "module disable $id"
    }
    val result = execKsud(cmd, true)
    Log.i(TAG, "$cmd result: $result")
    return result
}

fun uninstallModule(id: String): Boolean {
    val cmd = "module uninstall $id"
    val result = execKsud(cmd, true)
    Log.i(TAG, "uninstall module $id result: $result")
    return result
}

fun restoreModule(id: String): Boolean {
    val cmd = "module restore $id"
    val result = execKsud(cmd, true)
    Log.i(TAG, "restore module $id result: $result")
    return result
}

fun undoUninstallModule(id: String): Boolean {
    val cmd = "module undo-uninstall $id"
    val result = execKsud(cmd, true)
    Log.i(TAG, "undo uninstall module $id result: $result")
    return result
}

private fun flashWithIO(
    cmd: String,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit
): Shell.Result {

    val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStdout(s ?: "")
        }
    }

    val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStderr(s ?: "")
        }
    }

    // Set TMPDIR to app cache directory so ksud can create temp files without root
    val tmpDir = ksuApp.cacheDir.absolutePath
    val cmdWithEnv = "TMPDIR=$tmpDir $cmd"

    return withNewRootShell {
        newJob().add(cmdWithEnv).to(stdoutCallback, stderrCallback).exec()
    }
}

private fun shellQuoteArgument(value: String): String =
    "'${value.replace("'", "'\\''")}'"

fun flashAnyKernel3(
    zipPath: String,
    targetSlot: String?,
    useMkbootfs: Boolean,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit,
): Boolean {
    var success = false
    var exitCode = 1
    try {
        val command = buildString {
            append("flash ak3 ")
            append(shellQuoteArgument(zipPath))
            targetSlot?.let {
                append(" --slot ")
                append(shellQuoteArgument(it))
            }
            if (useMkbootfs) {
                append(" --use-mkbootfs")
            }
        }
        val result = flashWithIO(
            ksudCmd(command),
            onStdout = { output -> output.lineSequence().forEach(onStdout) },
            onStderr = { output -> output.lineSequence().forEach(onStderr) },
        )
        success = result.isSuccess
        exitCode = result.code
        Log.i(TAG, "AnyKernel3 flash result: success=$success, code=$exitCode")
    } catch (error: Exception) {
        Log.e(TAG, "Failed to flash AnyKernel3 package", error)
        onStderr(error.message ?: "Unknown AnyKernel3 error")
    } finally {
        File(zipPath).delete()
        onFinish(success, exitCode)
    }
    return success
}

fun flashModule(
    uri: Uri,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit
): Boolean {
    val resolver = ksuApp.contentResolver
    with(resolver.openInputStream(uri)) {
        val file = File(ksuApp.cacheDir, "module.zip")
        file.outputStream().use { output ->
            this?.copyTo(output)
        }
        val cmd = "module install ${file.absolutePath}"
        val result = flashWithIO(ksudCmd(cmd), onStdout, onStderr)
        Log.i("KernelSU", "install module $uri result: $result")

        file.delete()

        onFinish(result.isSuccess, result.code)
        return result.isSuccess
    }
}

fun runModuleAction(
    moduleId: String, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStdout(s ?: "")
        }
    }

    val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStderr(s ?: "")
        }
    }

    val result = withNewRootShell(true) {
        newJob().add(ksudCmd("module action $moduleId"))
            .to(stdoutCallback, stderrCallback).exec()
    }
    Log.i("KernelSU", "Module runAction result: $result")

    return result.isSuccess
}

fun restoreBoot(
    onFinish: (Boolean, Int) -> Unit, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val ksudPath = getKsuDaemonPath()
    val result = flashWithIO(
        ksudCmd("boot-restore -f --magiskboot $ksudPath"),
        onStdout,
        onStderr
    )
    onFinish(result.isSuccess, result.code)
    return result.isSuccess
}

fun uninstallPermanently(
    onFinish: (Boolean, Int) -> Unit, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val ksudPath = getKsuDaemonPath()
    val result =
        flashWithIO(ksudCmd("uninstall --magiskboot $ksudPath"), onStdout, onStderr)
    onFinish(result.isSuccess, result.code)
    return result.isSuccess
}

@Parcelize
sealed class LkmSelection : Parcelable {
    data class LkmUri(val uri: Uri) : LkmSelection()
    data class KmiString(val value: String) : LkmSelection()
    data object KmiNone : LkmSelection()
}

fun installBoot(
    bootUri: Uri?,
    lkm: LkmSelection,
    ota: Boolean,
    partition: String?,
    allowShell: Boolean = false,
    enableAdb: Boolean = false,
    forceBackup: Boolean = false,
    superKey: String? = null,
    signatureBypass: Boolean = false,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit,
): Boolean {
    val pendingUtsBoot = getPendingUtsBootTemplate()
    val pendingUtsBootExists = hasPendingUtsBootConfigFile()
    if (pendingUtsBootExists && pendingUtsBoot == null) {
        onStderr(ksuApp.getString(R.string.uts_view_boot_invalid_patch_blocked))
        onFinish(false, 1)
        return false
    }
    if (pendingUtsBoot != null && !savePendingUtsBootTemplate(pendingUtsBoot)) {
        onStderr(ksuApp.getString(R.string.uts_view_boot_invalid_patch_blocked))
        onFinish(false, 1)
        return false
    }
    val patchedUtsBootDraft = pendingUtsBoot ?: getSavedUtsBootDraft() ?: UtsTemplate()
    val patchedUtsBootToken = utsBootConfigurationToken(
        enabled = pendingUtsBoot != null,
        template = patchedUtsBootDraft,
    )

    val resolver = ksuApp.contentResolver

    val bootFile = bootUri?.let { uri ->
        with(resolver.openInputStream(uri)) {
            val bootFile = File(ksuApp.cacheDir, "boot.img")
            bootFile.outputStream().use { output ->
                this?.copyTo(output)
            }

            bootFile
        }
    }

    val ksudPath = getKsuDaemonPath()
    var cmd = "boot-patch --magiskboot $ksudPath"

    cmd += if (bootFile == null) {
        // no boot.img, use -f to force install
        " -f"
    } else {
        " -b ${bootFile.absolutePath}"
    }

    if (ota) {
        cmd += " -u"
    }

    if (forceBackup) {
        cmd += " --backup"
    }

    // Add superkey if specified
    if (!superKey.isNullOrBlank()) {
        cmd += " --superkey \"$superKey\""
        // Add signature bypass flag if enabled
        if (signatureBypass) {
            cmd += " --signature-bypass"
        }
    }

    var lkmFile: File? = null
    when (lkm) {
        is LkmSelection.LkmUri -> {
            lkmFile = with(resolver.openInputStream(lkm.uri)) {
                val file = File(ksuApp.cacheDir, "kernelsu-tmp-lkm.ko")
                file.outputStream().use { output ->
                    this?.copyTo(output)
                }

                file
            }
            cmd += " -m ${lkmFile.absolutePath}"
        }

        is LkmSelection.KmiString -> {
            cmd += " --kmi ${lkm.value}"
        }

        LkmSelection.KmiNone -> {
            // do nothing
        }
    }

    // output dir
    val downloadsDir =
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
    cmd += " -o $downloadsDir"

    partition?.let { part ->
        cmd += " --partition $part"
    }

    if (allowShell) {
        cmd += " --allow-shell"
    }

    if (enableAdb) {
        cmd += " --enable-adbd"
    }

    if (pendingUtsBoot != null) {
        val file = utsBootConfigFile()
        cmd += " --uts-config ${shellQuoteArgument(file.absolutePath)}"
    }

    val result = flashWithIO(ksudCmd(cmd), onStdout, onStderr)
    Log.i("KernelSU", "install boot result: ${result.isSuccess}")
    if (
        result.isSuccess &&
        !recordPatchedUtsBootConfigurationToken(patchedUtsBootToken)
    ) {
        Log.e("KernelSU", "Failed to persist patched UTS boot configuration token")
    }

    bootFile?.delete()
    lkmFile?.delete()
    // if boot uri is empty, it is direct install, when success, we should show reboot button
    onFinish(bootUri == null && result.isSuccess, result.code)

    if (bootUri == null && result.isSuccess) {
        install()
    }

    return result.isSuccess
}

fun restartAdbd() {
    ShellUtils.fastCmd(getRootShell(), "setprop ctl.restart adbd")
}

fun reboot(reason: String = "") {
    val shell = getRootShell()
    if (reason == "soft_reboot") {
        ShellUtils.fastCmd(shell, "setprop ctl.restart zygote")
        return
    }
    if (reason == "recovery") {
        // KEYCODE_POWER = 26, hide incorrect "Factory data reset" message
        ShellUtils.fastCmd(shell, "/system/bin/input keyevent 26")
    }
    ShellUtils.fastCmd(shell, "/system/bin/svc power reboot $reason || /system/bin/reboot $reason")
}

fun rootAvailable(): Boolean {
    val shell = getRootShell()
    return shell.isRoot
}


suspend fun getCurrentKmi(): String = withContext(Dispatchers.IO) {
    ksudReadString("boot-info current-kmi")
        .takeIf { it.matches(Regex("""(?:android\d+-)?\d+\.\d+""")) }
        .orEmpty()
}

suspend fun getSupportedKmis(): List<String> = withContext(Dispatchers.IO) {
    ksudReadLines("boot-info supported-kmis")
}

suspend fun isAbDevice(): Boolean = withContext(Dispatchers.IO) {
    ksudReadString("boot-info is-ab-device").toBoolean()
}

suspend fun getDefaultPartition(): String = withContext(Dispatchers.IO) {
    if (getRootShell().isRoot) {
        ksudReadString("boot-info default-partition")
            .takeIf { it == "boot" || it == "init_boot" || it == "vendor_boot" }
            .orEmpty()
    } else {
        // Partition auto-selection is a flashing decision. Without root we
        // cannot obtain UTS View's immutable original identity, so fail closed.
        ""
    }
}

suspend fun getSlotSuffix(ota: Boolean): String = withContext(Dispatchers.IO) {
    val args = if (ota) "boot-info slot-suffix --ota" else "boot-info slot-suffix"
    ksudReadString(args)
}

suspend fun getAvailablePartitions(): List<String> = withContext(Dispatchers.IO) {
    ksudReadLines("boot-info available-partitions")
        .filter { it == "boot" || it == "init_boot" || it == "vendor_boot" }
        .distinct()
}

fun hasMagisk(): Boolean {
    val shell = getRootShell(true)
    val result = shell.newJob().add("which magisk").exec()
    Log.i(TAG, "has magisk: ${result.isSuccess}")
    return result.isSuccess
}

fun isSepolicyValid(rules: String?): Boolean {
    if (rules == null) return true
    return execKsud("sepolicy check '$rules'")
}

fun getSepolicy(pkg: String): String =
    ksudReadLines("profile get-sepolicy $pkg").joinToString("\n")

fun setSepolicy(pkg: String, rules: String): Boolean {
    val ok = execKsud("profile set-sepolicy $pkg '$rules'")
    Log.i(TAG, "set sepolicy $pkg result: $ok")
    return ok
}

fun listAppProfileTemplates(): List<String> =
    ksudReadLines("profile list-templates")

fun getAppProfileTemplate(id: String): String =
    ksudReadLines("profile get-template '$id'").joinToString("\n")

fun setAppProfileTemplate(id: String, template: String): Boolean {
    val escapedTemplate = template.replace("\"", "\\\"")
    return execKsud("""profile set-template "$id" "$escapedTemplate"""")
}

fun deleteAppProfileTemplate(id: String): Boolean =
    execKsud("profile delete-template '$id'")

fun forceStopApp(packageName: String) {
    val shell = getRootShell()
    val result = shell.newJob().add("am force-stop $packageName").exec()
    Log.i(TAG, "force stop $packageName result: $result")
}

fun launchApp(packageName: String) {

    val shell = getRootShell()
    val result =
        shell.newJob()
            .add("cmd package resolve-activity --brief $packageName | tail -n 1 | xargs cmd activity start-activity -n")
            .exec()
    Log.i(TAG, "launch $packageName result: $result")
}

fun restartApp(packageName: String) {
    forceStopApp(packageName)
    launchApp(packageName)
}


fun runCmd(shell: Shell, cmd: String): String {
    return shell.newJob()
        .add(cmd)
        .to(mutableListOf<String>(), null)
        .exec().out
        .joinToString("\n")
}


fun getMetaModuleImplement(): String {
    try {
        val metaModuleProp = SuFile.open("/data/adb/metamodule/module.prop")
        if (!metaModuleProp.isFile) {
            Log.i(TAG, "Meta module implement: None")
            return "None"
        }

        val prop = Properties()
        prop.load(metaModuleProp.newInputStream())

        val name = prop.getProperty("name")
        Log.i(TAG, "Meta module implement: $name")
        return name
    } catch (_ : Throwable) {
        Log.i(TAG, "Meta module implement: None")
        return "None"
    }
}

/** Module IDs of known third-party Zygisk implementations ("zygisksu" covers
 *  both ZygiskNext and NeoZygisk -- they share that id). "yukizygisk" is the
 *  standalone module; built-in YukiZygisk remains a kernel feature detected by
 *  its flag. These modules are force-disabled while the built-in feature is on. */
val ZYGISK_IMPL_MODULE_IDS = listOf("zygisksu", "rezygisk", "yukizygisk")

private const val YUKIZYGISK_STANDALONE_MODULE_ID = "yukizygisk"
private const val YUKIZYGISK_STANDALONE_DISPLAY_NAME = "YukiZygisk-Standalone"

suspend fun getZygiskImplement(): String = withContext(Dispatchers.IO) {
    // Built-in YukiZygisk wins: it's a kernel feature, not a /data/adb module.
    if (getFeatureValue("yukizygisk")) return@withContext "YukiZygisk"

    for (moduleId in ZYGISK_IMPL_MODULE_IDS) {
        // skip disabled / pending-removal modules
        if (SuFile.open("/data/adb/modules/$moduleId/disable").isFile || SuFile.open("/data/adb/modules/$moduleId/remove").isFile) continue

        val propFile = SuFile.open("/data/adb/modules/$moduleId/module.prop")
        if (!propFile.isFile) continue

        val prop = Properties()
        prop.load(propFile.newInputStream())

        val name = if (moduleId == YUKIZYGISK_STANDALONE_MODULE_ID) {
            YUKIZYGISK_STANDALONE_DISPLAY_NAME
        } else {
            prop.getProperty("name")
        }
        Log.i(TAG, "Zygisk implement: $name")
        return@withContext name
    }

    Log.i(TAG, "Zygisk implement: None")
    "None"
}

fun addUmountPath(path: String, flags: Int): Boolean {
    val shell = getRootShell()
    val flagsArg = if (flags >= 0) "--flags $flags" else ""
    val cmd = ksudCmd("umount add $path $flagsArg")
    val result = ShellUtils.fastCmdResult(shell, cmd)
    Log.i(TAG, "add umount path $path result: $result")
    return result
}
fun removeUmountPath(path: String): Boolean {
    val shell = getRootShell()
    val cmd = ksudCmd("umount remove $path")
    val result = ShellUtils.fastCmdResult(shell, cmd)
    Log.i(TAG, "remove umount path $path result: $result")
    return result
}

fun listUmountPaths(): String {
    val shell = getRootShell()
    val cmd = ksudCmd("umount list")
    return try {
        runCmd(shell, cmd).trim()
    } catch (e: Exception) {
        Log.e(TAG, "Failed to list umount paths", e)
        ""
    }
}

fun clearCustomUmountPaths(): Boolean {
    val shell = getRootShell()
    val cmd = ksudCmd("umount clear-custom")
    val result = ShellUtils.fastCmdResult(shell, cmd)
    Log.i(TAG, "clear custom umount paths result: $result")
    return result
}

fun saveUmountConfig(): Boolean {
    val shell = getRootShell()
    val cmd = ksudCmd("umount save")
    val result = ShellUtils.fastCmdResult(shell, cmd)
    Log.i(TAG, "save umount config result: $result")
    return result
}

fun applyUmountConfigToKernel(): Boolean {
    val shell = getRootShell()
    val cmd = ksudCmd("umount apply")
    val result = ShellUtils.fastCmdResult(shell, cmd)
    Log.i(TAG, "apply umount config to kernel result: $result")
    return result
}

data class PluginCommandResult(
    val exitCode: Int,
    val stdout: String,
    val stderr: String,
) {
    val isSuccess: Boolean
        get() = exitCode == 0

    val output: String
        get() = listOf(stdout, stderr).filter { it.isNotBlank() }.joinToString("\n")
}

private fun runPluginCommand(args: String, newShell: Boolean = false): PluginCommandResult =
    runCatching {
        val stdout = ArrayList<String>()
        val stderr = ArrayList<String>()
        val result = if (newShell) {
            withNewRootShell {
                newJob().add(ksudCmd(args)).to(stdout, stderr).exec()
            }
        } else {
            getRootShell().newJob().add(ksudCmd(args)).to(stdout, stderr).exec()
        }
        PluginCommandResult(
            exitCode = result.code,
            stdout = stdout.joinToString("\n"),
            stderr = stderr.joinToString("\n"),
        )
    }.getOrElse { error ->
        Log.e(TAG, "Plugin command failed", error)
        PluginCommandResult(-1, "", error.message.orEmpty())
    }

fun listPlugins(): PluginCommandResult = runPluginCommand("plugin list")

fun togglePlugin(id: String, enable: Boolean): Boolean {
    val operation = if (enable) "enable" else "disable"
    return runPluginCommand(
        "plugin $operation ${shellQuoteArgument(id)}",
        newShell = true,
    ).isSuccess
}

fun uninstallPlugin(id: String): Boolean =
    runPluginCommand(
        "plugin uninstall ${shellQuoteArgument(id)}",
        newShell = true,
    ).isSuccess

fun runPluginCallback(id: String, function: String): PluginCommandResult =
    runPluginCommand(
        "plugin run ${shellQuoteArgument(id)} ${shellQuoteArgument(function)}",
        newShell = true,
    )

fun runPluginAction(id: String): PluginCommandResult =
    runPluginCommand(
        "plugin action ${shellQuoteArgument(id)}",
        newShell = true,
    )

fun getPluginLog(id: String): PluginCommandResult =
    runPluginCommand("plugin log ${shellQuoteArgument(id)}")

fun clearPluginLog(id: String): Boolean =
    runPluginCommand(
        "plugin clear-log ${shellQuoteArgument(id)}",
        newShell = true,
    ).isSuccess

fun getPluginConfig(id: String, key: String): PluginCommandResult =
    runPluginCommand(
        "plugin config --id ${shellQuoteArgument(id)} get ${shellQuoteArgument(key)}",
    )

fun savePluginConfig(id: String, key: String, value: String): Boolean =
    runPluginCommand(
        "plugin config --id ${shellQuoteArgument(id)} set " +
            "${shellQuoteArgument(key)} ${shellQuoteArgument(value)}",
        newShell = true,
    ).isSuccess

fun deletePluginConfig(id: String, key: String): Boolean =
    runPluginCommand(
        "plugin config --id ${shellQuoteArgument(id)} delete ${shellQuoteArgument(key)}",
        newShell = true,
    ).isSuccess

fun listPluginConfig(id: String): PluginCommandResult =
    runPluginCommand("plugin config --id ${shellQuoteArgument(id)} list")

fun installPluginZip(zipPath: String): PluginCommandResult {
    val zipFile = File(zipPath)
    return try {
        runPluginCommand(
            "plugin install ${shellQuoteArgument(zipPath)}",
            newShell = true,
        )
    } finally {
        if (!zipFile.delete() && zipFile.exists()) {
            Log.w(TAG, "Failed to delete plugin installation cache file")
        }
    }
}
