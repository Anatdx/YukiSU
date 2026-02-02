package ui.screen.kernelFlash.state

import android.app.Activity
import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.util.rootAvailable
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.io.File
import java.io.FileOutputStream


/**
 * @author ShirkNeko
 * @date 2025/5/31.
 */
data class FlashState(
    val isFlashing: Boolean = false,
    val isCompleted: Boolean = false,
    val progress: Float = 0f,
    val currentStep: String = "",
    val logs: List<String> = emptyList(),
    val error: String = ""
)

class HorizonKernelState {
    private val _state = MutableStateFlow(FlashState())
    val state: StateFlow<FlashState> = _state.asStateFlow()

    fun updateProgress(progress: Float) {
        _state.update { it.copy(progress = progress) }
    }

    fun updateStep(step: String) {
        _state.update { it.copy(currentStep = step) }
    }

    fun addLog(log: String) {
        _state.update {
            it.copy(logs = it.logs + log)
        }
    }

    fun setError(error: String) {
        _state.update { it.copy(error = error) }
    }

    fun startFlashing() {
        _state.update {
            it.copy(
                isFlashing = true,
                isCompleted = false,
                progress = 0f,
                currentStep = "Preparing...",
                logs = emptyList(),
                error = ""
            )
        }
    }

    fun completeFlashing() {
        _state.update { it.copy(isCompleted = true, progress = 1f) }
    }

    fun reset() {
        _state.value = FlashState()
    }
}

/**
 * Simplified kernel flash worker that uses ksud flash command
 */
class HorizonKernelWorker(
    private val context: Context,
    private val state: HorizonKernelState,
    private val slot: String? = null
) : Thread() {
    var uri: Uri? = null
    private var onFlashComplete: (() -> Unit)? = null

    fun setOnFlashCompleteListener(listener: () -> Unit) {
        onFlashComplete = listener
    }

    override fun run() {
        state.startFlashing()
        state.updateStep(context.getString(R.string.horizon_preparing))

        try {
            // Check root
            if (!rootAvailable()) {
                state.setError(context.getString(R.string.root_required))
                return
            }

            state.updateProgress(0.1f)
            state.updateStep(context.getString(R.string.horizon_copying_files))

            // Copy zip file to KSU temp directory (accessible by ksud)
            val fileName = DocumentFile.fromSingleUri(context, uri!!)?.name ?: "kernel.zip"
            val tempDir = "/data/adb/ksu/tmp"
            val zipPath = "$tempDir/$fileName"
            
            // Create temp dir and copy file with root
            Shell.cmd("mkdir -p $tempDir").exec()
            
            // First copy to app's cache, then move with root
            val cacheFile = File(context.cacheDir, fileName)
            context.contentResolver.openInputStream(uri!!)?.use { input ->
                FileOutputStream(cacheFile).use { output ->
                    input.copyTo(output)
                }
            }
            
            // Move to KSU tmp with proper permissions
            val copyResult = Shell.cmd(
                "cp '${cacheFile.absolutePath}' '$zipPath'",
                "chmod 644 '$zipPath'"
            ).exec()
            
            // Clean up cache file
            cacheFile.delete()
            
            if (!copyResult.isSuccess) {
                state.setError(context.getString(R.string.horizon_copy_failed))
                return
            }

            state.addLog("Copied: $zipPath")
            state.updateProgress(0.2f)

            // Build ksud flash command
            val cmdBuilder = StringBuilder("ksud flash ak3 \"$zipPath\" -v")

            // Add slot option for A/B devices
            if (slot != null) {
                cmdBuilder.append(" --slot $slot")
                state.addLog("Target slot: $slot")
            }

            // Save log to file
            val logFile = "/data/adb/ksu/tmp/ak3_flash.log"
            cmdBuilder.append(" --log \"$logFile\"")

            state.updateStep(context.getString(R.string.horizon_flashing))
            state.updateProgress(0.3f)
            state.addLog("Executing: $cmdBuilder")

            // Execute ksud flash command with stderr merged
            val result = Shell.cmd("$cmdBuilder 2>&1").exec()
            
            state.addLog("Exit code: ${result.code}")

            // Parse output (now includes both stdout and stderr)
            result.out.forEach { line ->
                state.addLog(line)

                // Update progress based on output
                when {
                    line.contains("Extracting", ignoreCase = true) -> {
                        state.updateProgress(0.5f)
                        state.updateStep("Extracting...")
                    }
                    line.contains("Installing", ignoreCase = true) ||
                    line.contains("Flashing", ignoreCase = true) -> {
                        state.updateProgress(0.7f)
                        state.updateStep("Flashing kernel...")
                    }
                    line.contains("complete", ignoreCase = true) ||
                    line.contains("Done", ignoreCase = true) -> {
                        state.updateProgress(0.9f)
                        state.updateStep("Completing...")
                    }
                }
            }

            // Check result
            if (result.isSuccess) {
                state.updateStep(context.getString(R.string.horizon_flash_complete_status))
                state.completeFlashing()

                (context as? Activity)?.runOnUiThread {
                    onFlashComplete?.invoke()
                }
            } else {
                val errorMsg = result.err.joinToString("\n").ifEmpty {
                    context.getString(R.string.flash_failed_message)
                }
                state.setError(errorMsg)
            }

        } catch (e: Exception) {
            state.setError(e.message ?: context.getString(R.string.horizon_unknown_error))
        } finally {
            // Cleanup temp file
            try {
                Shell.cmd("rm -rf /data/adb/ksu/tmp/ak3_flash /data/adb/ksu/tmp/*.zip /data/adb/ksu/tmp/ak3_flash.log").exec()
            } catch (_: Exception) {}
        }
    }
}