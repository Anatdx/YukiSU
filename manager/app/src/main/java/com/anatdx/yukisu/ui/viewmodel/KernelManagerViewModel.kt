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
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.util.getKsud
import com.anatdx.yukisu.ui.util.getRootShell
import com.topjohnwu.superuser.Shell
import org.json.JSONObject
import android.util.Log
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
                    val shell = getRootShell()
                    val ksud = getKsud()

                    // Helper to run a command and check for errors
                    val runCmd = { cmd: String ->
                        val stdout = mutableListOf<String>()
                        val stderr = mutableListOf<String>()
                        val result = shell.newJob().add(cmd).to(stdout, stderr).exec()
                        Log.d("KernelManager", "CMD: '$cmd' | CODE: ${result.code}")
                        Log.d("KernelManager", "STDOUT: ${stdout.joinToString("\n")}")
                        Log.d("KernelManager", "STDERR: ${stderr.joinToString("\n")}")
                        if (!result.isSuccess) {
                            throw Exception(stderr.joinToString("\n").ifEmpty { "Command failed with exit code ${result.code}" })
                        }
                        stdout
                    }

                    // Get boot slot info
                    val bootInfoStdout = runCmd("$ksud flash boot-info")
                    val bootInfoJson = bootInfoStdout.joinToString("")
                    if (bootInfoJson.isNotEmpty()) {
                        val bootInfo = JSONObject(bootInfoJson)
                        hasOtherSlot = bootInfo.optBoolean("is_ab", false)
                        if (hasOtherSlot) {
                            currentSlot = bootInfo.optString("current_slot", "")
                            otherSlot = bootInfo.optString("other_slot", "")
                        }
                    }

                    // Get current kernel version
                    val kernelOutput = runCmd("$ksud flash kernel").joinToString("\n").trim()
                    currentKernelVersion = kernelOutput.substringAfter(":", kernelOutput).trim()

                    // Get other slot kernel version
                    if (hasOtherSlot && otherSlot.isNotEmpty()) {
                        val otherKernelOutput = runCmd("$ksud flash kernel --slot $otherSlot").joinToString("\n").trim()
                        otherKernelVersion = otherKernelOutput.substringAfter(":", otherKernelOutput).trim()
                    }

                    // Get AVB status
                    val avbOutput = runCmd("$ksud flash avb").joinToString("\n").trim()
                    avbStatus = avbOutput.substringAfter(":", avbOutput).trim()

                } catch (e: Exception) {
                    Log.e("KernelManager", "Failed to load kernel info", e)
                    // Reset fields on error to avoid showing stale data
                    currentSlot = ""
                    otherSlot = ""
                    hasOtherSlot = false
                    currentKernelVersion = "Error"
                    otherKernelVersion = "Error"
                    avbStatus = "Error"
                }
            }
            isLoading = false
        }
    }

    suspend fun flashKernelImage(context: Context, uri: Uri): Result<String> = withContext(Dispatchers.IO) {
        try {
            val bootPartition = detectBootPartition()
            val tempFile = File(context.cacheDir, "kernel_temp.img")
            context.contentResolver.openInputStream(uri)?.use { input ->
                tempFile.outputStream().use { output -> input.copyTo(output) }
            }

            val shell = getRootShell()
            val ksud = getKsud()
            val stdout = mutableListOf<String>()
            val stderr = mutableListOf<String>()

            // CORRECTED ORDER: <image> <partition>
            val command = "$ksud flash image ${tempFile.absolutePath} $bootPartition"
            Log.i("KernelManager", "Executing: $command")

            val execResult = shell.newJob()
                .add(command)
                .to(stdout, stderr)
                .exec()
            tempFile.delete()

            Log.i("KernelManager", "CMD: '$command' | EXIT: ${execResult.code}")
            Log.i("KernelManager", "STDOUT: ${stdout.joinToString("\n")}")
            Log.i("KernelManager", "STDERR: ${stderr.joinToString("\n")}")

            if (execResult.isSuccess) {
                Result.success(context.getString(R.string.kernel_flash_success))
            } else {
                Result.failure(Exception(stderr.joinToString("\n").ifEmpty { stdout.joinToString("\n") }))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun flashAK3(context: Context, uri: Uri): Result<String> = withContext(Dispatchers.IO) {
        try {
            val tempFile = File(context.cacheDir, "ak3_temp.zip")
            context.contentResolver.openInputStream(uri)?.use { input ->
                tempFile.outputStream().use { output -> input.copyTo(output) }
            }

            val shell = getRootShell()
            val ksud = getKsud()
            val stdout = mutableListOf<String>()
            val stderr = mutableListOf<String>()

            val command = "$ksud flash ak3 ${tempFile.absolutePath}"
            Log.i("KernelManager", "Executing: $command")

            val execResult = shell.newJob()
                .add(command)
                .to(stdout, stderr)
                .exec()
            tempFile.delete()

            Log.i("KernelManager", "CMD: '$command' | EXIT: ${execResult.code}")
            Log.i("KernelManager", "STDOUT: ${stdout.joinToString("\n")}")
            Log.i("KernelManager", "STDERR: ${stderr.joinToString("\n")}")

            if (execResult.isSuccess) {
                Result.success(context.getString(R.string.kernel_flash_success))
            } else {
                Result.failure(Exception(stderr.joinToString("\n").ifEmpty { stdout.joinToString("\n") }))
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

            val shell = getRootShell()
            val ksud = getKsud()
            val stdout = mutableListOf<String>()
            val stderr = mutableListOf<String>()

            val command = "$ksud flash backup $bootPartition ${outputFile.absolutePath}"
            Log.i("KernelManager", "Executing: $command")

            val execResult = shell.newJob()
                .add(command)
                .to(stdout, stderr)
                .exec()

            Log.i("KernelManager", "CMD: '$command' | EXIT: ${execResult.code}")
            Log.i("KernelManager", "STDOUT: ${stdout.joinToString("\n")}")
            Log.i("KernelManager", "STDERR: ${stderr.joinToString("\n")}")

            if (execResult.isSuccess && outputFile.exists()) {
                Result.success(outputFile.absolutePath)
            } else {
                Result.failure(Exception(stderr.joinToString("\n").ifEmpty { stdout.joinToString("\n") }))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun flashModule(context: Context, uri: Uri): Result<String> = withContext(Dispatchers.IO) {
        try {
            val tempFile = File(context.cacheDir, "module_temp.ko")
            context.contentResolver.openInputStream(uri)?.use { input ->
                tempFile.outputStream().use { output -> input.copyTo(output) }
            }

            val shell = getRootShell()
            val ksud = getKsud()
            val stdout = mutableListOf<String>()
            val stderr = mutableListOf<String>()

            val command = "$ksud module install ${tempFile.absolutePath}"
            Log.i("KernelManager", "Executing: $command")

            val execResult = shell.newJob()
                .add(command)
                .to(stdout, stderr)
                .exec()
            tempFile.delete()

            Log.i("KernelManager", "CMD: '$command' | EXIT: ${execResult.code}")
            Log.i("KernelManager", "STDOUT: ${stdout.joinToString("\n")}")
            Log.i("KernelManager", "STDERR: ${stderr.joinToString("\n")}")

            if (execResult.isSuccess) {
                Result.success(context.getString(R.string.kernel_flash_success))
            } else {
                Result.failure(Exception(stderr.joinToString("\n").ifEmpty { stdout.joinToString("\n") }))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun disableAvb(): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            val ksud = getKsud()
            val stdout = mutableListOf<String>()
            val stderr = mutableListOf<String>()

            val command = "$ksud flash avb disable"
            Log.i("KernelManager", "Executing: $command")

            val execResult = shell.newJob()
                .add(command)
                .to(stdout, stderr)
                .exec()

            Log.i("KernelManager", "CMD: '$command' | EXIT: ${execResult.code}")
            Log.i("KernelManager", "STDOUT: ${stdout.joinToString("\n")}")
            Log.i("KernelManager", "STDERR: ${stderr.joinToString("\n")}")

            if (execResult.isSuccess) {
                Result.success(Unit)
            } else {
                Result.failure(Exception(stderr.joinToString("\n").ifEmpty { stdout.joinToString("\n") }))
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
}
