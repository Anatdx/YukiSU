package ui.screen.partition

import android.content.Context
import android.util.Log
import com.anatdx.yukisu.ui.util.KsuCli
import com.anatdx.yukisu.ui.util.getRootShell
import com.anatdx.yukisu.ui.util.hasMagiskBoot
import com.topjohnwu.superuser.CallbackList
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 分区管理助手
 * 通过 ksud 命令获取分区信息
 */
object PartitionManagerHelper {
    private const val TAG = "PartitionManagerHelper"
    
    /**
     * 获取槽位信息
     */
    suspend fun getSlotInfo(context: Context): SlotInfo? = withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            val result = mutableListOf<String>()
            
            // 执行 ksud flash slots 命令
            shell.newJob()
                .add("${KsuCli.getKsuDaemonPath()} flash slots")
                .to(result)
                .exec()
            
            if (result.isEmpty()) {
                // 不是 A/B 设备
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
    
    /**
     * 获取分区列表
     */
    suspend fun getPartitionList(context: Context, slot: String?): List<PartitionInfo> = withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            val result = mutableListOf<String>()
            
            // 执行 ksud flash list 命令
            val cmd = if (slot != null) {
                "${KsuCli.getKsuDaemonPath()} flash list --slot $slot"
            } else {
                "${KsuCli.getKsuDaemonPath()} flash list"
            }
            
            shell.newJob()
                .add(cmd)
                .to(result)
                .exec()
            
            val partitions = mutableListOf<PartitionInfo>()
            
            result.forEach { line ->
                // 解析输出格式：  boot                 [logical, 67108864 bytes]
                if (line.trim().startsWith("[") || line.contains("Available partitions")) {
                    return@forEach
                }
                
                val trimmed = line.trim()
                if (trimmed.isEmpty()) return@forEach
                
                try {
                    // 提取分区名称
                    val parts = trimmed.split(Regex("\\s+"), limit = 2)
                    if (parts.size < 2) return@forEach
                    
                    val name = parts[0]
                    val info = parts[1]
                    
                    // 提取类型和大小
                    val typeMatch = Regex("\\[(.*?),\\s*(\\d+)\\s*bytes\\]").find(info)
                    if (typeMatch != null) {
                        val type = typeMatch.groupValues[1]
                        val size = typeMatch.groupValues[2].toLongOrNull() ?: 0L
                        val isLogical = type == "logical"
                        
                        // 获取块设备路径
                        val blockDevice = getPartitionBlockDevice(shell, name, slot)
                        
                        partitions.add(
                            PartitionInfo(
                                name = name,
                                blockDevice = blockDevice,
                                type = type,
                                size = size,
                                isLogical = isLogical
                            )
                        )
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
    
    /**
     * 获取分区的块设备路径
     */
    private fun getPartitionBlockDevice(shell: Shell, partition: String, slot: String?): String {
        val result = mutableListOf<String>()
        
        val cmd = if (slot != null) {
            "${KsuCli.getKsuDaemonPath()} flash info $partition --slot $slot"
        } else {
            "${KsuCli.getKsuDaemonPath()} flash info $partition"
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
    
    /**
     * 备份分区
     */
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
                "${KsuCli.getKsuDaemonPath()} flash backup $partition $outputPath --slot $slot"
            } else {
                "${KsuCli.getKsuDaemonPath()} flash backup $partition $outputPath"
            }
            
            val stdout = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let { onStdout(it) }
                }
            }
            
            val stderr = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let { onStderr(it) }
                }
            }
            
            val result = shell.newJob()
                .add(cmd)
                .to(stdout, stderr)
                .exec()
            
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to backup partition", e)
            onStderr("Error: ${e.message}")
            false
        }
    }
    
    /**
     * 刷写分区
     */
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
                "${KsuCli.getKsuDaemonPath()} flash image $imagePath $partition --slot $slot"
            } else {
                "${KsuCli.getKsuDaemonPath()} flash image $imagePath $partition"
            }
            
            val stdout = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let { onStdout(it) }
                }
            }
            
            val stderr = object : CallbackList<String>() {
                override fun onAddElement(s: String?) {
                    s?.let { onStderr(it) }
                }
            }
            
            val result = shell.newJob()
                .add(cmd)
                .to(stdout, stderr)
                .exec()
            
            result.isSuccess
        } catch (e: Exception) {
            Log.e(TAG, "Failed to flash partition", e)
            onStderr("Error: ${e.message}")
            false
        }
    }
}
