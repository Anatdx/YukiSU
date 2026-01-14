package com.anatdx.yukisu.ui.viewmodel

import android.content.Context
import android.net.Uri
import android.os.Environment
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.ksuApp
import com.anatdx.yukisu.R
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class KernelManagerViewModel : ViewModel() {
    
    var isLoading by mutableStateOf(true)
        private set
    
    var currentSlot by mutableStateOf("")
        private set
    
    var otherSlot by mutableStateOf("")
        private set
    
    var hasOtherSlot by mutableStateOf(false)
        private set
    
    var currentKernelVersion by mutableStateOf("")
        private set
    
    var otherKernelVersion by mutableStateOf("")
        private set
    
    var avbStatus by mutableStateOf("")
        private set
    
    fun loadKernelInfo() {
        viewModelScope.launch {
            isLoading = true
            withContext(Dispatchers.IO) {
                try {
                    // Get boot slot info
                    val bootInfoJson = execKsudCommand("flash boot-info")
                    if (bootInfoJson.isNotEmpty()) {
                        val bootInfo = JSONObject(bootInfoJson)
                        val isAb = bootInfo.optBoolean("is_ab", false)
                        hasOtherSlot = isAb
                        
                        if (isAb) {
                            currentSlot = bootInfo.optString("current_slot", "")
                            otherSlot = bootInfo.optString("other_slot", "")
                        }
                    }
                    
                    // Get current kernel version
                    val kernelOutput = execKsudCommand("flash kernel").trim()
                    // Parse "Kernel version: Linux version ..." -> "Linux version ..."
                    currentKernelVersion = if (kernelOutput.contains(":")) {
                        kernelOutput.substringAfter(":").trim()
                    } else {
                        kernelOutput
                    }
                    
                    // Get other slot kernel version if exists
                    if (hasOtherSlot && otherSlot.isNotEmpty()) {
                        val otherKernelOutput = execKsudCommand("flash kernel --slot $otherSlot").trim()
                        otherKernelVersion = if (otherKernelOutput.contains(":")) {
                            otherKernelOutput.substringAfter(":").trim()
                        } else {
                            otherKernelOutput
                        }
                    }
                    
                    // Get AVB status
                    val avbOutput = execKsudCommand("flash avb").trim()
                    // Parse "AVB/dm-verity status: enabled" -> "enabled"
                    avbStatus = if (avbOutput.contains(":")) {
                        avbOutput.substringAfter(":").trim()
                    } else {
                        avbOutput
                    }
                    
                } catch (e: Exception) {
                    e.printStackTrace()
                }
            }
            isLoading = false
        }
    }
    
    suspend fun flashAK3(context: Context, uri: Uri): Result<String> = withContext(Dispatchers.IO) {
        try {
            // Copy AK3 zip to temp location
            val tempFile = File(context.cacheDir, "ak3_temp.zip")
            context.contentResolver.openInputStream(uri)?.use { input ->
                tempFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            
            // Flash using ksud
            val output = execKsudCommand("flash ak3 ${tempFile.absolutePath}")
            tempFile.delete()
            
            if (output.contains("success", ignoreCase = true) || output.contains("complete", ignoreCase = true)) {
                Result.success(context.getString(R.string.kernel_flash_success))
            } else {
                Result.failure(Exception(output))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }
    
    suspend fun flashKernelImage(context: Context, uri: Uri): Result<String> = withContext(Dispatchers.IO) {
        try {
            // Determine partition name (boot or init_boot)
            val bootPartition = detectBootPartition()
            
            // Flash using ksud flash image
            val tempFile = File(context.cacheDir, "kernel_temp.img")
            context.contentResolver.openInputStream(uri)?.use { input ->
                tempFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            
            val output = execKsudCommand("flash image $bootPartition ${tempFile.absolutePath}")
            tempFile.delete()
            
            if (output.contains("success", ignoreCase = true) || output.isEmpty()) {
                Result.success(context.getString(R.string.kernel_flash_success))
            } else {
                Result.failure(Exception(output))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }
    
    suspend fun extractKernel(context: Context): Result<String> = withContext(Dispatchers.IO) {
        try {
            val bootPartition = detectBootPartition()
            val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
            val fileName = "kernel_${bootPartition}${currentSlot}_$timestamp.img"
            val outputDir = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "KernelSU")
            outputDir.mkdirs()
            val outputFile = File(outputDir, fileName)
            
            // Use ksud flash backup
            val output = execKsudCommand("flash backup $bootPartition ${outputFile.absolutePath}")
            
            if (outputFile.exists()) {
                Result.success(outputFile.absolutePath)
            } else {
                Result.failure(Exception(output))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }
    
    suspend fun flashModule(context: Context, uri: Uri): Result<String> = withContext(Dispatchers.IO) {
        try {
            val tempFile = File(context.cacheDir, "module_temp.ko")
            context.contentResolver.openInputStream(uri)?.use { input ->
                tempFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            
            // Flash using ksud module install
            val output = execKsudCommand("module install ${tempFile.absolutePath}")
            tempFile.delete()
            
            if (output.contains("success", ignoreCase = true) || output.contains("installed", ignoreCase = true)) {
                Result.success(context.getString(R.string.kernel_flash_success))
            } else {
                Result.failure(Exception(output))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }
    
    suspend fun disableAvb(): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            val output = execKsudCommand("flash avb disable")
            
            if (output.contains("success", ignoreCase = true) || output.isEmpty()) {
                Result.success(Unit)
            } else {
                Result.failure(Exception(output))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }
    
    private fun detectBootPartition(): String {
        // Try to detect init_boot first (GKI 2.0), then fall back to boot
        val initBootExists = File("/dev/block/by-name/init_boot$currentSlot").exists() ||
                            File("/dev/block/by-name/init_boot").exists()
        return if (initBootExists) "init_boot" else "boot"
    }
    
    private fun execKsudCommand(command: String): String {
        return try {
            val process = Runtime.getRuntime().exec(arrayOf("su", "-c", "ksud $command"))
            val output = process.inputStream.bufferedReader().readText()
            val error = process.errorStream.bufferedReader().readText()
            process.waitFor()
            
            if (error.isNotEmpty() && !error.contains("warning", ignoreCase = true)) {
                error
            } else {
                output
            }
        } catch (e: Exception) {
            e.printStackTrace()
            ""
        }
    }
}
