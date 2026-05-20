package ui.screen.partition

import android.content.Context
import android.util.Log
import com.anatdx.yukisu.ui.util.getRootShell
import com.anatdx.yukisu.ui.util.ksudCmd
import com.anatdx.yukisu.ui.util.ksudReadLines
import com.topjohnwu.superuser.CallbackList
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

object PartitionManagerHelper {
    private const val TAG = "PartitionManagerHelper"

    private fun shellQuote(value: String): String =
        "'${value.replace("'", "'\\''")}'"

    suspend fun getSlotInfo(context: Context): SlotInfo? = withContext(Dispatchers.IO) {
        try {
            val lines = ksudReadLines("flash slots")
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
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get slot info", e)
            null
        }
    }

    suspend fun getPartitionList(context: Context, slot: String?, scanAll: Boolean = false): List<PartitionInfo> = withContext(Dispatchers.IO) {
        try {
            val args = buildString {
                append("flash list")
                if (slot != null) append(" --slot $slot")
                if (scanAll) append(" --all")
            }
            val lines = ksudReadLines(args)
            val typeRegex = Regex("\\[(.*?),\\s*(\\d+)\\s*bytes\\]")
            lines.mapNotNull { line ->
                // Sample line: "  boot                 [logical, 67108864 bytes] [DANGEROUS]"
                if (line.startsWith("[") || "partitions" in line) return@mapNotNull null
                val parts = line.split(Regex("\\s+"), limit = 2)
                if (parts.size < 2) return@mapNotNull null
                val name = parts[0]
                val info = parts[1]
                val typeMatch = typeRegex.find(info) ?: return@mapNotNull null
                val type = typeMatch.groupValues[1]
                val size = typeMatch.groupValues[2].toLongOrNull() ?: 0L
                PartitionInfo(
                    name = name,
                    blockDevice = getPartitionBlockDevice(name, slot),
                    type = type,
                    size = size,
                    isLogical = type == "logical",
                    isDangerous = "[DANGEROUS]" in info,
                    // userdata/data are excluded from batch backup; logical filtering happens in UI
                    excludeFromBatch = name == "userdata" || name == "data",
                )
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get partition list", e)
            emptyList()
        }
    }

    private fun getPartitionBlockDevice(partition: String, slot: String?): String {
        val args = if (slot != null) "flash info $partition --slot $slot" else "flash info $partition"
        return ksudReadLines(args)
            .firstOrNull { "Block device:" in it }
            ?.substringAfter("Block device:")
            ?.trim()
            .orEmpty()
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
            
            val cmd = ksudCmd("flash map $slot")
            
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
