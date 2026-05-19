package ui.screen.partition

import android.content.Context
import android.util.Log
import com.anatdx.yukisu.ui.util.getKsud
import com.anatdx.yukisu.ui.util.getRootShell
import com.topjohnwu.superuser.CallbackList
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

object PartitionManagerHelper {
    private const val TAG = "PartitionManagerHelper"

    private fun shellQuote(value: String): String {
        return "'${value.replace("'", "'\\''")}'"
    }
    
    suspend fun getSlotInfo(context: Context): SlotInfo? = withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            val result = mutableListOf<String>()
            
            val cmd = "${getKsud()} flash slots"
            Log.d(TAG, "Executing command: $cmd")
            val execResult = shell.newJob()
                .add(cmd)
                .to(result)
                .exec()
            
            Log.d(TAG, "Slots command result: success=${execResult.isSuccess}, output lines=${result.size}")
            result.forEach { line ->
                Log.d(TAG, "Slots output: $line")
            }
            
            if (result.isEmpty()) {
                return@withContext SlotInfo(
                    isAbDevice = false,
                    currentSlot = null,
                    otherSlot = null
                )
            }
            
            var isAbDevice = true
            var currentSlot: String? = null
            var otherSlot: String? = null
            
            result.forEach { line ->
                when {
                    line.contains("This device is not A/B partitioned") -> {
                        isAbDevice = false
                    }
                    line.contains("Current slot:") -> {
                        currentSlot = line.substringAfter("Current slot:").trim()
                    }
                    line.contains("Other slot:") -> {
                        otherSlot = line.substringAfter("Other slot:").trim()
                    }
                }
            }
            
            SlotInfo(
                isAbDevice = isAbDevice,
                currentSlot = currentSlot,
                otherSlot = otherSlot
            )
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get slot info", e)
            null
        }
    }
    
    suspend fun getPartitionList(context: Context, slot: String?, scanAll: Boolean = false): List<PartitionInfo> = withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            val result = mutableListOf<String>()
            
            val cmdParts = mutableListOf("${getKsud()} flash list")
            if (slot != null) {
                cmdParts.add("--slot $slot")
            }
            if (scanAll) {
                cmdParts.add("--all")
            }
            val cmd = cmdParts.joinToString(" ")
            
            Log.d(TAG, "Executing command: $cmd")
            val execResult = shell.newJob()
                .add(cmd)
                .to(result)
                .exec()
            
            Log.d(TAG, "Command result: success=${execResult.isSuccess}, output lines=${result.size}")
            result.forEach { line ->
                Log.d(TAG, "Output line: $line")
            }
            
            val partitions = mutableListOf<PartitionInfo>()
            
            result.forEach { line ->
                // 解析输出格式：  boot                 [logical, 67108864 bytes] [DANGEROUS]
                Log.d(TAG, "Parsing line: '$line'")
                if (line.trim().startsWith("[") || line.contains("partitions")) {
                    Log.d(TAG, "Skipping header line")
                    return@forEach
                }
                
                val trimmed = line.trim()
                if (trimmed.isEmpty()) {
                    Log.d(TAG, "Skipping empty line")
                    return@forEach
                }
                
                try {
                    val parts = trimmed.split(Regex("\\s+"), limit = 2)
                    Log.d(TAG, "Split into ${parts.size} parts: ${parts.joinToString(" | ")}")
                    if (parts.size < 2) {
                        Log.d(TAG, "Insufficient parts, skipping")
                        return@forEach
                    }
                    
                    val name = parts[0]
                    val info = parts[1]
                    Log.d(TAG, "Partition name: '$name', info: '$info'")
                    
                    val isDangerous = info.contains("[DANGEROUS]")
                    
                    val typeMatch = Regex("\\[(.*?),\\s*(\\d+)\\s*bytes\\]").find(info)
                    if (typeMatch != null) {
                        val type = typeMatch.groupValues[1]
                        val sizeStr = typeMatch.groupValues[2]
                        val size = sizeStr.toLongOrNull() ?: 0L
                        val isLogical = type == "logical"
                        
                        // 只有 userdata 和 data 排除在批量备份之外（逻辑分区在 UI 中单独过滤）
                        val excludeFromBatch = name == "userdata" || name == "data"
                        
                        Log.d(TAG, "Matched type: '$type', size string: '$sizeStr', size: $size, dangerous: $isDangerous")
                        
                        val blockDevice = getPartitionBlockDevice(shell, name, slot)
                        Log.d(TAG, "Block device: '$blockDevice'")
                        
                        val partitionInfo = PartitionInfo(
                            name = name,
                            blockDevice = blockDevice,
                            type = type,
                            size = size,
                            isLogical = isLogical,
                            isDangerous = isDangerous,
                            excludeFromBatch = excludeFromBatch
                        )
                        partitions.add(partitionInfo)
                        Log.d(TAG, "Added partition: $name, size=$size, type=$type, dangerous=$isDangerous")
                    } else {
                        Log.w(TAG, "Failed to match regex pattern for line: '$info'")
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to parse partition line: $line", e)
                }
            }
            
            partitions
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get partition list", e)
            emptyList()
        }
    }
    
    private fun getPartitionBlockDevice(shell: Shell, partition: String, slot: String?): String {
        val result = mutableListOf<String>()
        
        val cmd = if (slot != null) {
            "${getKsud()} flash info $partition --slot $slot"
        } else {
            "${getKsud()} flash info $partition"
        }
        
        shell.newJob()
            .add(cmd)
            .to(result)
            .exec()
        
        result.forEach { line ->
            if (line.contains("Block device:")) {
                return line.substringAfter("Block device:").trim()
            }
        }
        
        return ""
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
            
            val cmd = if (slot != null) {
                "${getKsud()} flash backup ${shellQuote(partition)} ${shellQuote(outputPath)} --slot ${shellQuote(slot)}"
            } else {
                "${getKsud()} flash backup ${shellQuote(partition)} ${shellQuote(outputPath)}"
            }
            
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
            
            val cmd = if (slot != null) {
                "${getKsud()} flash image ${shellQuote(imagePath)} ${shellQuote(partition)} --slot ${shellQuote(slot)}"
            } else {
                "${getKsud()} flash image ${shellQuote(imagePath)} ${shellQuote(partition)}"
            }
            
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
            
            val cmd = "${getKsud()} flash map $slot"
            
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
