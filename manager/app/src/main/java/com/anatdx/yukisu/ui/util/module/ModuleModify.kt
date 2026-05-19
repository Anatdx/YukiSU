package com.anatdx.yukisu.ui.util.module

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalContext
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ksu.KsuPaths
import com.anatdx.yukisu.ui.util.reboot
import com.topjohnwu.superuser.io.SuFileInputStream
import com.topjohnwu.superuser.io.SuFileOutputStream
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.*

object ModuleModify {
    @Composable
    fun RestoreConfirmationDialog(
        showDialog: Boolean,
        onConfirm: () -> Unit,
        onDismiss: () -> Unit
    ) {
        val context = LocalContext.current

        if (showDialog) {
            AlertDialog(
                onDismissRequest = onDismiss,
                title = {
                    Text(
                        text = context.getString(R.string.restore_confirm_title),
                        style = MaterialTheme.typography.headlineSmall
                    )
                },
                text = {
                    Text(
                        text = context.getString(R.string.restore_confirm_message),
                        style = MaterialTheme.typography.bodyMedium
                    )
                },
                confirmButton = {
                    TextButton(onClick = onConfirm) {
                        Text(context.getString(R.string.confirm))
                    }
                },
                dismissButton = {
                    TextButton(onClick = onDismiss) {
                        Text(context.getString(R.string.cancel))
                    }
                }
            )
        }
    }

    @Composable
    fun AllowlistRestoreConfirmationDialog(
        showDialog: Boolean,
        onConfirm: () -> Unit,
        onDismiss: () -> Unit
    ) {
        val context = LocalContext.current

        if (showDialog) {
            AlertDialog(
                onDismissRequest = onDismiss,
                title = {
                    Text(
                        text = context.getString(R.string.allowlist_restore_confirm_title),
                        style = MaterialTheme.typography.headlineSmall
                    )
                },
                text = {
                    Text(
                        text = context.getString(R.string.allowlist_restore_confirm_message),
                        style = MaterialTheme.typography.bodyMedium
                    )
                },
                confirmButton = {
                    TextButton(onClick = onConfirm) {
                        Text(context.getString(R.string.confirm))
                    }
                },
                dismissButton = {
                    TextButton(onClick = onDismiss) {
                        Text(context.getString(R.string.cancel))
                    }
                }
            )
        }
    }

    suspend fun backupModules(context: Context, snackBarHost: SnackbarHostState, uri: Uri) {
        withContext(Dispatchers.IO) {
            try {
                // busybox tar streams the module tree to stdout; we pipe that into the user-chosen URI.
                val process = ProcessBuilder("su", "-c", "cd ${KsuPaths.MODULES_DIR} && ${KsuPaths.BUSYBOX} tar -cz .")
                    .redirectErrorStream(false)
                    .start()

                context.contentResolver.openOutputStream(uri)?.use { output ->
                    process.inputStream.copyTo(output)
                } ?: throw IOException("Failed to open output uri")

                val error = process.errorStream.bufferedReader().readText()
                if (process.waitFor() != 0) {
                    throw IOException(context.getString(R.string.command_execution_failed, error))
                }

                withContext(Dispatchers.Main) {
                    snackBarHost.showSnackbar(
                        context.getString(R.string.backup_success),
                        duration = SnackbarDuration.Long
                    )
                }

            } catch (e: Exception) {
                Log.e("Backup", context.getString(R.string.backup_failed, ""), e)
                withContext(Dispatchers.Main) {
                    snackBarHost.showSnackbar(
                        context.getString(R.string.backup_failed, e.message),
                        duration = SnackbarDuration.Long
                    )
                }
            }
        }
    }

    suspend fun restoreModules(
        context: Context,
        snackBarHost: SnackbarHostState,
        uri: Uri,
        showConfirmDialog: (Boolean) -> Unit,
        confirmResult: CompletableDeferred<Boolean>
    ) {
        withContext(Dispatchers.Main) {
            showConfirmDialog(true)
        }

        val userConfirmed = confirmResult.await()
        if (!userConfirmed) return

        withContext(Dispatchers.IO) {
            try {
                val process = ProcessBuilder("su", "-c", "${KsuPaths.BUSYBOX} tar -xz -C ${KsuPaths.MODULES_DIR}")
                    .redirectErrorStream(false)
                    .start()

                context.contentResolver.openInputStream(uri)?.use { input ->
                    process.outputStream.use { output ->
                        input.copyTo(output)
                    }
                } ?: throw IOException("Failed to open input uri")

                val error = process.errorStream.bufferedReader().readText()
                if (process.waitFor() != 0) {
                    throw IOException(context.getString(R.string.command_execution_failed, error))
                }

                withContext(Dispatchers.Main) {
                    val snackbarResult = snackBarHost.showSnackbar(
                        message = context.getString(R.string.restore_success),
                        actionLabel = context.getString(R.string.restart_now),
                        duration = SnackbarDuration.Long
                    )
                    if (snackbarResult == SnackbarResult.ActionPerformed) {
                        reboot()
                    }
                }

            } catch (e: Exception) {
                Log.e("Restore", context.getString(R.string.restore_failed, ""), e)
                withContext(Dispatchers.Main) {
                    snackBarHost.showSnackbar(
                        message = context.getString(
                            R.string.restore_failed,
                            e.message ?: context.getString(R.string.unknown_error)
                        ),
                        duration = SnackbarDuration.Long
                    )
                }
            }
        }
    }

    suspend fun backupAllowlist(context: Context, snackBarHost: SnackbarHostState, uri: Uri) {
        withContext(Dispatchers.IO) {
            try {
                SuFileInputStream.open(KsuPaths.ALLOWLIST).use { input ->
                    context.contentResolver.openOutputStream(uri)?.use { output ->
                        input.copyTo(output)
                    } ?: throw IOException("Failed to open output uri")
                }

                withContext(Dispatchers.Main) {
                    snackBarHost.showSnackbar(
                        context.getString(R.string.allowlist_backup_success),
                        duration = SnackbarDuration.Long
                    )
                }

            } catch (e: Exception) {
                Log.e("AllowlistBackup", context.getString(R.string.allowlist_backup_failed, ""), e)
                withContext(Dispatchers.Main) {
                    snackBarHost.showSnackbar(
                        context.getString(R.string.allowlist_backup_failed, e.message),
                        duration = SnackbarDuration.Long
                    )
                }
            }
        }
    }

    suspend fun restoreAllowlist(
        context: Context,
        snackBarHost: SnackbarHostState,
        uri: Uri,
        showConfirmDialog: (Boolean) -> Unit,
        confirmResult: CompletableDeferred<Boolean>
    ) {
        withContext(Dispatchers.Main) {
            showConfirmDialog(true)
        }

        val userConfirmed = confirmResult.await()
        if (!userConfirmed) return

        withContext(Dispatchers.IO) {
            try {
                context.contentResolver.openInputStream(uri)?.use { input ->
                    SuFileOutputStream.open(KsuPaths.ALLOWLIST).use { output ->
                        input.copyTo(output)
                    }
                } ?: throw IOException("Failed to open input uri")

                withContext(Dispatchers.Main) {
                    snackBarHost.showSnackbar(
                        context.getString(R.string.allowlist_restore_success),
                        duration = SnackbarDuration.Long
                    )
                }

            } catch (e: Exception) {
                Log.e(
                    "AllowlistRestore",
                    context.getString(R.string.allowlist_restore_failed, ""),
                    e
                )
                withContext(Dispatchers.Main) {
                    snackBarHost.showSnackbar(
                        context.getString(R.string.allowlist_restore_failed, e.message),
                        duration = SnackbarDuration.Long
                    )
                }
            }
        }
    }

    @Composable
    fun rememberModuleBackupLauncher(
        context: Context,
        snackBarHost: SnackbarHostState,
        scope: CoroutineScope = rememberCoroutineScope()
    ) = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK) {
            result.data?.data?.let { uri ->
                scope.launch {
                    backupModules(context, snackBarHost, uri)
                }
            }
        }
    }

    @Composable
    fun rememberModuleRestoreLauncher(
        context: Context,
        snackBarHost: SnackbarHostState,
        scope: CoroutineScope = rememberCoroutineScope()
    ): ActivityResultLauncher<Intent> {
        var showRestoreDialog by remember { mutableStateOf(false) }
        var restoreConfirmResult by remember { mutableStateOf<CompletableDeferred<Boolean>?>(null) }

        RestoreConfirmationDialog(
            showDialog = showRestoreDialog,
            onConfirm = {
                showRestoreDialog = false
                restoreConfirmResult?.complete(true)
            },
            onDismiss = {
                showRestoreDialog = false
                restoreConfirmResult?.complete(false)
            }
        )

        return rememberLauncherForActivityResult(
            contract = ActivityResultContracts.StartActivityForResult()
        ) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                result.data?.data?.let { uri ->
                    scope.launch {
                        val confirmResult = CompletableDeferred<Boolean>()
                        restoreConfirmResult = confirmResult

                        restoreModules(
                            context = context,
                            snackBarHost = snackBarHost,
                            uri = uri,
                            showConfirmDialog = { show -> showRestoreDialog = show },
                            confirmResult = confirmResult
                        )
                    }
                }
            }
        }
    }

    @Composable
    fun rememberAllowlistBackupLauncher(
        context: Context,
        snackBarHost: SnackbarHostState,
        scope: CoroutineScope = rememberCoroutineScope()
    ) = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK) {
            result.data?.data?.let { uri ->
                scope.launch {
                    backupAllowlist(context, snackBarHost, uri)
                }
            }
        }
    }

    @Composable
    fun rememberAllowlistRestoreLauncher(
        context: Context,
        snackBarHost: SnackbarHostState,
        scope: CoroutineScope = rememberCoroutineScope()
    ): ActivityResultLauncher<Intent> {
        var showAllowlistRestoreDialog by remember { mutableStateOf(false) }
        var allowlistRestoreConfirmResult by remember {
            mutableStateOf<CompletableDeferred<Boolean>?>(
                null
            )
        }

        AllowlistRestoreConfirmationDialog(
            showDialog = showAllowlistRestoreDialog,
            onConfirm = {
                showAllowlistRestoreDialog = false
                allowlistRestoreConfirmResult?.complete(true)
            },
            onDismiss = {
                showAllowlistRestoreDialog = false
                allowlistRestoreConfirmResult?.complete(false)
            }
        )

        return rememberLauncherForActivityResult(
            contract = ActivityResultContracts.StartActivityForResult()
        ) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                result.data?.data?.let { uri ->
                    scope.launch {
                        val confirmResult = CompletableDeferred<Boolean>()
                        allowlistRestoreConfirmResult = confirmResult

                        restoreAllowlist(
                            context = context,
                            snackBarHost = snackBarHost,
                            uri = uri,
                            showConfirmDialog = { show -> showAllowlistRestoreDialog = show },
                            confirmResult = confirmResult
                        )
                    }
                }
            }
        }
    }

    fun createBackupIntent(): Intent {
        return Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/zip"
            val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
            putExtra(Intent.EXTRA_TITLE, "modules_backup_$timestamp.zip")
        }
    }

    fun createRestoreIntent(): Intent {
        return Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/zip"
        }
    }

    fun createAllowlistBackupIntent(): Intent {
        return Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/octet-stream"
            val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
            putExtra(Intent.EXTRA_TITLE, "ksu_allowlist_backup_$timestamp.dat")
        }
    }

    fun createAllowlistRestoreIntent(): Intent {
        return Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/octet-stream"
        }
    }
}