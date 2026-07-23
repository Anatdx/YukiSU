package ui.screen.partition

import android.content.Context
import android.util.Log
import com.anatdx.yukisu.ui.util.getRootShell
import com.anatdx.yukisu.ui.util.ksudCmd
import com.topjohnwu.superuser.CallbackList
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.IOException

object PartitionManagerHelper {
    private const val TAG = "PartitionManagerHelper"

    private fun shellQuote(value: String): String =
        "'${value.replace("'", "'\\''")}'"

    private fun runKsudLines(args: String): List<String> {
        val stdout = ArrayList<String>()
        val stderr = ArrayList<String>()
        val result = getRootShell()
            .newJob()
            .add(ksudCmd(args))
            .to(stdout, stderr)
            .exec()
        if (!result.isSuccess) {
            throw IOException(
                stderr.lastOrNull()
                    ?: stdout.lastOrNull()
                    ?: "ksud exited with code ${result.code}"
            )
        }
        return stdout.filter { it.isNotBlank() }.map { it.trim() }
    }

    suspend fun getSlotInfo(context: Context): SlotInfo? = withContext(Dispatchers.IO) {
        val lines = runKsudLines("flash slots")
        if (lines.isEmpty()) {
            return@withContext SlotInfo(isAbDevice = false, currentSlot = null, otherSlot = null)
        }
        var isAbDevice = true
        var currentSlot: String? = null
        var otherSlot: String? = null
        lines.forEach { line ->
            when {
                "This device is not A/B partitioned" in line -> isAbDevice = false
                "Current slot:" in line -> currentSlot = line.substringAfter("Current slot:").trim()
                "Other slot:" in line -> otherSlot = line.substringAfter("Other slot:").trim()
            }
        }
        SlotInfo(isAbDevice, currentSlot, otherSlot)
    }

    suspend fun getPartitionList(context: Context, slot: String?, scanAll: Boolean = false): List<PartitionInfo> = withContext(Dispatchers.IO) {
        val args = buildString {
            append("flash list")
            if (slot != null) append(" --slot ${shellQuote(slot)}")
            if (scanAll) append(" --all")
        }
        parsePartitionList(runKsudLines(args))
    }

    /**
     * Resolve the block device lazily. Resolving it for every list row used to
     * launch one extra root command per partition and made "Show all" painfully slow.
     */
    suspend fun getPartitionBlockDevice(
        context: Context,
        partition: String,
        slot: String?
    ): String = withContext(Dispatchers.IO) {
        val args = buildString {
            append("flash info ${shellQuote(partition)}")
            if (slot != null) append(" --slot ${shellQuote(slot)}")
        }
        runKsudLines(args)
            .firstOrNull { "Block device:" in it }
            ?.substringAfter("Block device:")
            ?.trim()
            .orEmpty()
    }

    internal fun parsePartitionList(lines: List<String>): List<PartitionInfo> {
        val typeRegex = Regex("\\[([^,]+),\\s*(\\d+)\\s*bytes\\]")
        return lines.mapNotNull { rawLine ->
            // Sample line: "boot                 [logical, 67108864 bytes] [DANGEROUS]"
            val line = rawLine.trim()
            if (line.startsWith("[") || line.contains("partitions", ignoreCase = true)) {
                return@mapNotNull null
            }
            val typeMatch = typeRegex.find(line) ?: return@mapNotNull null
            val name = line.substring(0, typeMatch.range.first).trim()
            if (name.isEmpty()) return@mapNotNull null
            val type = typeMatch.groupValues[1].trim()
            val size = typeMatch.groupValues[2].toLongOrNull() ?: return@mapNotNull null
            PartitionInfo(
                name = name,
                blockDevice = "",
                type = type,
                size = size,
                isLogical = type.equals("logical", ignoreCase = true),
                isDangerous = "[DANGEROUS]" in line,
                excludeFromBatch = name == "userdata" || name == "data",
            )
        }.distinctBy { it.name }
    }

    internal suspend fun inspectAk3Package(
        context: Context,
        zipPath: String,
    ): Ak3PackageInfo = withContext(Dispatchers.IO) {
        val values = runKsudLines("flash ak3-info ${shellQuote(zipPath)}")
            .mapNotNull { line ->
                val separator = line.indexOf('=')
                if (separator <= 0) null else {
                    line.substring(0, separator) to line.substring(separator + 1)
                }
            }
            .toMap()
        if (values["valid"] != "1") {
            throw IOException("ksud did not recognize this AnyKernel3 package")
        }
        Ak3PackageInfo(
            kernelName = values["kernel"].orEmpty(),
            devices = values["devices"]
                .orEmpty()
                .split('|')
                .filter(String::isNotBlank),
            packageSlotPolicy = values["slot_policy"]?.takeIf(String::isNotBlank),
        )
    }

    suspend fun backupPartition(
        context: Context,
        partition: String,
        outputPath: String,
        slot: String?,
        onStdout: (String) -> Unit = {},
        onStderr: (String) -> Unit = {}
    ): Boolean = withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            
            val slotArg = if (slot != null) " --slot ${shellQuote(slot)}" else ""
            val cmd = ksudCmd("flash backup ${shellQuote(partition)} ${shellQuote(outputPath)}$slotArg")
            
            Log.d(TAG, "Executing backup command: $cmd")
            
            val stdout = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let {
                        Log.d(TAG, "Backup stdout: $it")
                        onStdout(it)
                    }
                }
            }
            
            val stderr = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let {
                        Log.e(TAG, "Backup stderr: $it")
                        onStderr(it)
                    }
                }
            }
            
            val result = shell.newJob()
                .add(cmd)
                .to(stdout, stderr)
                .exec()
            
            Log.d(TAG, "Backup result: success=${result.isSuccess}, code=${result.code}")
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to backup partition", e)
            onStderr("Error: ${e.message}")
            false
        }
    }
    
    suspend fun flashPartition(
        context: Context,
        imagePath: String,
        partition: String,
        slot: String?,
        onStdout: (String) -> Unit = {},
        onStderr: (String) -> Unit = {}
    ): Boolean = withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            
            val slotArg = if (slot != null) " --slot ${shellQuote(slot)}" else ""
            val cmd = ksudCmd("flash image ${shellQuote(imagePath)} ${shellQuote(partition)}$slotArg")
            
            Log.d(TAG, "Executing flash command: $cmd")
            
            val stdout = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let {
                        Log.d(TAG, "Flash stdout: $it")
                        onStdout(it)
                    }
                }
            }
            
            val stderr = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let {
                        Log.e(TAG, "Flash stderr: $it")
                        onStderr(it)
                    }
                }
            }
            
            val result = shell.newJob()
                .add(cmd)
                .to(stdout, stderr)
                .exec()
            
            Log.d(TAG, "Flash result: success=${result.isSuccess}, code=${result.code}")
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to flash partition", e)
            onStderr("Error: ${e.message}")
            false
        }
    }
    
    /**
     * 映射逻辑分区（用于非活跃槽位）
     */
    suspend fun mapLogicalPartitions(
        context: Context,
        slot: String,
        onStdout: (String) -> Unit = {},
        onStderr: (String) -> Unit = {}
    ): Boolean = withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            
            val cmd = ksudCmd("flash map ${shellQuote(slot)}")
            
            Log.d(TAG, "Executing map command: $cmd")
            
            val stdout = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let {
                        Log.d(TAG, "Map stdout: $it")
                        onStdout(it)
                    }
                }
            }
            
            val stderr = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let {
                        Log.e(TAG, "Map stderr: $it")
                        onStderr(it)
                    }
                }
            }
            
            val result = shell.newJob()
                .add(cmd)
                .to(stdout, stderr)
                .exec()
            
            Log.d(TAG, "Map result: success=${result.isSuccess}, code=${result.code}")
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to map logical partitions", e)
            onStderr("Error: ${e.message}")
            false
        }
    }
}
