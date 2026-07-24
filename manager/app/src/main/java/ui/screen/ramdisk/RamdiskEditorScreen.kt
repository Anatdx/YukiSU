package ui.screen.ramdisk

import android.content.Context
import android.net.Uri
import android.os.SystemClock
import android.provider.OpenableColumns
import android.text.format.Formatter
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Save
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.IconButton
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.anatdx.yukifb.backend.FileContentSource
import com.anatdx.yukifb.model.EntryId
import com.anatdx.yukifb.model.FileEntry
import com.anatdx.yukifb.model.FileEntryType
import com.anatdx.yukifb.state.FileBrowserAction
import com.anatdx.yukifb.state.TextFileEditorState
import com.anatdx.yukifb.state.rememberFileBrowserController
import com.anatdx.yukifb.ui.FileBrowser
import com.anatdx.yukifb.ui.TextFileEditor
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.getKsud
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ui.screen.partition.PartitionManagerHelper
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun RamdiskEditorScreen(
    navigator: DestinationsNavigator,
    partitionName: String,
    targetSlot: String?,
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val snackbarHost = remember { SnackbarHostState() }
    var retryGeneration by remember { mutableIntStateOf(0) }
    var loadState by remember {
        mutableStateOf<RamdiskEditorLoadState>(RamdiskEditorLoadState.Loading)
    }
    var selectedFragmentIndex by remember { mutableStateOf<Int?>(null) }
    var pendingImportDirectory by remember { mutableStateOf<EntryId?>(null) }
    var pendingExportEntry by remember { mutableStateOf<FileEntry?>(null) }
    var importGeneration by remember { mutableIntStateOf(0) }
    var isImporting by remember { mutableStateOf(false) }
    var isExportingFile by remember { mutableStateOf(false) }
    var isDumping by remember { mutableStateOf(false) }
    var hasRebuiltImage by remember { mutableStateOf(false) }
    var hasUnexportedImage by remember { mutableStateOf(false) }
    var openedTextFile by remember { mutableStateOf<OpenedTextFile?>(null) }
    var isOpeningTextFile by remember { mutableStateOf(false) }
    var isSavingTextFile by remember { mutableStateOf(false) }
    var discardPrompt by remember { mutableStateOf<DiscardPrompt?>(null) }

    LaunchedEffect(partitionName, targetSlot, retryGeneration) {
        (loadState as? RamdiskEditorLoadState.Ready)?.image?.closeNow()
        loadState = RamdiskEditorLoadState.Loading
        selectedFragmentIndex = null
        loadState = runCatching {
            prepareRamdiskEditor(
                context = context,
                partitionName = partitionName,
                targetSlot = targetSlot,
            )
        }.getOrElse { error ->
            RamdiskEditorLoadState.Error(
                error.message ?: resources.getString(R.string.partition_unknown)
            )
        }
    }

    val ready = loadState as? RamdiskEditorLoadState.Ready
    DisposableEffect(ready) {
        onDispose {
            ready?.image?.closeNow()
            ready?.sourceImage?.delete()
            ready?.outputImage?.delete()
        }
    }

    val importLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
    ) { uri: Uri? ->
        val parentId = pendingImportDirectory
        pendingImportDirectory = null
        val backend = (loadState as? RamdiskEditorLoadState.Ready)?.image
            ?.fragments
            ?.getOrNull(selectedFragmentIndex ?: 0)
        if (uri != null && parentId != null && backend != null) {
            scope.launch {
                isImporting = true
                runCatching {
                    val name = withContext(Dispatchers.IO) {
                        queryDisplayName(context, uri)
                            ?: resources.getString(R.string.ramdisk_editor_import_fallback_name)
                    }
                    backend.importFile(
                        parentId = parentId,
                        name = name,
                        source = FileContentSource {
                            context.contentResolver.openInputStream(uri)
                                ?: throw IOException(
                                    resources.getString(R.string.ramdisk_editor_document_open_failed)
                                )
                        },
                    )
                }.onSuccess {
                    importGeneration++
                }.onFailure { error ->
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.ramdisk_editor_import_failed,
                            error.message ?: resources.getString(R.string.partition_unknown),
                        )
                    )
                }
                isImporting = false
            }
        }
    }

    val fileExportLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("application/octet-stream"),
    ) { uri: Uri? ->
        val entry = pendingExportEntry
        pendingExportEntry = null
        val backend = (loadState as? RamdiskEditorLoadState.Ready)?.image
            ?.fragments
            ?.getOrNull(selectedFragmentIndex ?: 0)
        if (uri != null && entry != null && backend != null) {
            scope.launch {
                isExportingFile = true
                runCatching {
                    withContext(Dispatchers.IO) {
                        context.contentResolver.openOutputStream(uri, "wt")?.use { output ->
                            backend.read(entry.id) { input ->
                                input.copyTo(output)
                            }
                        } ?: throw IOException(
                            resources.getString(R.string.ramdisk_editor_document_open_failed)
                        )
                    }
                }.onSuccess {
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.ramdisk_editor_file_export_success,
                            entry.name,
                        )
                    )
                }.onFailure { error ->
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.ramdisk_editor_file_export_failed,
                            error.message ?: resources.getString(R.string.partition_unknown),
                        )
                    )
                }
                isExportingFile = false
            }
        }
    }

    val imageExportLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("application/octet-stream"),
    ) { uri: Uri? ->
        val outputImage = (loadState as? RamdiskEditorLoadState.Ready)?.image?.outputImage
        if (uri != null && outputImage?.isFile == true) {
            scope.launch {
                runCatching {
                    withContext(Dispatchers.IO) {
                        context.contentResolver.openOutputStream(uri, "wt")?.use { output ->
                            outputImage.inputStream().buffered().use { input ->
                                input.copyTo(output)
                            }
                        } ?: throw IOException(
                            resources.getString(R.string.ramdisk_editor_document_open_failed)
                        )
                    }
                }.onSuccess {
                    hasUnexportedImage = false
                    snackbarHost.showSnackbar(
                        resources.getString(R.string.ramdisk_editor_export_success)
                    )
                }.onFailure { error ->
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.ramdisk_editor_export_failed,
                            error.message ?: resources.getString(R.string.partition_unknown),
                        )
                    )
                }
            }
        }
    }

    when (val state = loadState) {
        RamdiskEditorLoadState.Loading -> {
            RamdiskEditorStatusScreen(
                title = stringResource(R.string.ramdisk_editor_title, partitionName),
                message = stringResource(R.string.ramdisk_editor_preparing),
                onBack = { navigator.popBackStack() },
                loading = true,
            )
        }

        is RamdiskEditorLoadState.Error -> {
            RamdiskEditorStatusScreen(
                title = stringResource(R.string.ramdisk_editor_title, partitionName),
                message = state.message,
                onBack = { navigator.popBackStack() },
                loading = false,
                onRetry = { retryGeneration++ },
            )
        }

        is RamdiskEditorLoadState.Ready -> {
            val image = state.image
            val dirty by image.dirty.collectAsState()
            val multipleRamdisks = image.fragments.size > 1
            var lastRootBackAt by remember(image) { mutableLongStateOf(0L) }

            fun requestEditorExit() {
                if (dirty || hasUnexportedImage) {
                    discardPrompt = DiscardPrompt.ARCHIVE
                    return
                }
                val now = SystemClock.elapsedRealtime()
                if (
                    lastRootBackAt != 0L &&
                    now - lastRootBackAt in 0L..ROOT_EXIT_CONFIRM_INTERVAL_MILLIS
                ) {
                    snackbarHost.currentSnackbarData?.dismiss()
                    navigator.popBackStack()
                } else {
                    lastRootBackAt = now
                    scope.launch {
                        snackbarHost.currentSnackbarData?.dismiss()
                        snackbarHost.showSnackbar(
                            message = resources.getString(
                                R.string.ramdisk_editor_press_back_again
                            ),
                            duration = SnackbarDuration.Short,
                        )
                    }
                }
            }

            fun rebuildImage() {
                if (!dirty || isDumping) return
                scope.launch {
                    isDumping = true
                    runCatching { image.dump() }
                        .onSuccess {
                            hasRebuiltImage = true
                            hasUnexportedImage = true
                            snackbarHost.showSnackbar(
                                resources.getString(R.string.ramdisk_editor_rebuild_success)
                            )
                        }
                        .onFailure { error ->
                            snackbarHost.showSnackbar(
                                resources.getString(
                                    R.string.ramdisk_editor_rebuild_failed,
                                    error.message
                                        ?: resources.getString(R.string.partition_unknown),
                                )
                            )
                        }
                    isDumping = false
                }
            }

            val exportImage = {
                imageExportLauncher.launch(buildExportFileName(partitionName))
            }

            LaunchedEffect(dirty, hasUnexportedImage, selectedFragmentIndex) {
                lastRootBackAt = 0L
            }

            if (multipleRamdisks && selectedFragmentIndex == null) {
                BackHandler(onBack = ::requestEditorExit)
                RamdiskFragmentSelector(
                    partitionName = partitionName,
                    fragments = image.fragments,
                    dirty = dirty,
                    hasRebuiltImage = hasRebuiltImage,
                    isDumping = isDumping,
                    snackbarHost = snackbarHost,
                    onBack = ::requestEditorExit,
                    onSelect = { selectedFragmentIndex = it.fragment.index },
                    onDump = ::rebuildImage,
                    onExportImage = exportImage,
                )
            } else {
                val backend = image.fragments.getOrElse(selectedFragmentIndex ?: 0) {
                    image.fragments.first()
                }
                val browserController = rememberFileBrowserController(
                    backend = backend,
                    initialDirectory = backend.rootEntry,
                )
                val browserState by browserController.state.collectAsState()

                LaunchedEffect(importGeneration, backend) {
                    if (importGeneration > 0) {
                        browserController.dispatch(FileBrowserAction.Refresh)
                    }
                }

                LaunchedEffect(
                    browserState.currentDirectory.id,
                    browserState.selectedIds,
                ) {
                    lastRootBackAt = 0L
                }

                fun handleBrowserBack() {
                    val editor = openedTextFile
                    when {
                        editor?.state?.hasUnsavedChanges == true -> {
                            discardPrompt = DiscardPrompt.TEXT_FILE
                        }

                        editor != null -> openedTextFile = null
                        browserState.isBusy ||
                            isImporting ||
                            isExportingFile ||
                            isOpeningTextFile ||
                            isSavingTextFile ||
                            isDumping -> Unit

                        browserState.isSelectionMode -> {
                            browserController.dispatch(FileBrowserAction.ClearSelection)
                        }

                        browserState.breadcrumbs.size > 1 -> {
                            browserController.dispatch(FileBrowserAction.NavigateUp)
                        }

                        multipleRamdisks -> {
                            selectedFragmentIndex = null
                        }

                        else -> requestEditorExit()
                    }
                }

                BackHandler(onBack = ::handleBrowserBack)

                Box(Modifier.fillMaxSize()) {
                    val editor = openedTextFile
                    if (editor != null) {
                        TextFileEditor(
                            fileName = editor.entry.name,
                            state = editor.state,
                            onSave = { text ->
                                if (!isSavingTextFile) {
                                    scope.launch {
                                        isSavingTextFile = true
                                        runCatching {
                                            backend.replace(
                                                editor.entry.id,
                                                FileContentSource {
                                                    ByteArrayInputStream(
                                                        text.toByteArray(Charsets.UTF_8)
                                                    )
                                                },
                                            )
                                        }.onSuccess {
                                            editor.state.markSaved()
                                        }.onFailure { error ->
                                            snackbarHost.showSnackbar(
                                                resources.getString(
                                                    R.string.ramdisk_editor_text_save_failed,
                                                    error.message
                                                        ?: resources.getString(
                                                            R.string.partition_unknown
                                                        ),
                                                )
                                            )
                                        }
                                        isSavingTextFile = false
                                    }
                                }
                            },
                            onClose = { openedTextFile = null },
                            modifier = Modifier.fillMaxSize(),
                            isSaving = isSavingTextFile,
                        )
                    } else {
                        FileBrowser(
                            controller = browserController,
                            onOpenFile = { entry ->
                                if (!isOpeningTextFile) {
                                    scope.launch {
                                        isOpeningTextFile = true
                                        runCatching {
                                            require(entry.type == FileEntryType.REGULAR_FILE) {
                                                resources.getString(
                                                    R.string.ramdisk_editor_regular_files_only
                                                )
                                            }
                                            require((entry.size ?: 0L) <= MAX_TEXT_FILE_SIZE) {
                                                resources.getString(
                                                    R.string.ramdisk_editor_text_too_large
                                                )
                                            }
                                            val text = backend.read(entry.id) { input ->
                                                readUtf8Text(input)
                                            }
                                            OpenedTextFile(
                                                entry = entry,
                                                state = TextFileEditorState(text),
                                            )
                                        }.onSuccess { opened ->
                                            openedTextFile = opened
                                        }.onFailure { error ->
                                            snackbarHost.showSnackbar(
                                                resources.getString(
                                                    R.string.ramdisk_editor_open_failed,
                                                    error.message
                                                        ?: resources.getString(
                                                            R.string.partition_unknown
                                                        ),
                                                )
                                            )
                                        }
                                        isOpeningTextFile = false
                                    }
                                }
                            },
                            onImport = { directory ->
                                if (!isImporting) {
                                    pendingImportDirectory = directory.id
                                    importLauncher.launch(arrayOf("*/*"))
                                }
                            },
                            onExport = { entry ->
                                if (!isExportingFile) {
                                    pendingExportEntry = entry
                                    fileExportLauncher.launch(entry.name)
                                }
                            },
                            onClose = ::handleBrowserBack,
                            modifier = Modifier.fillMaxSize(),
                            topBarActions = {
                                RamdiskImageActions(
                                    dirty = dirty,
                                    hasRebuiltImage = hasRebuiltImage,
                                    isDumping = isDumping,
                                    onDump = ::rebuildImage,
                                    onExportImage = exportImage,
                                )
                            },
                        )
                    }

                    if (isImporting || isExportingFile || isOpeningTextFile) {
                        Box(
                            modifier = Modifier.fillMaxSize(),
                            contentAlignment = Alignment.Center,
                        ) {
                            CircularProgressIndicator()
                        }
                    }
                    SnackbarHost(
                        hostState = snackbarHost,
                        modifier = Modifier
                            .align(Alignment.BottomCenter)
                            .padding(16.dp),
                    )
                }
            }
        }
    }

    when (discardPrompt) {
        DiscardPrompt.ARCHIVE -> {
            DiscardDialog(
                title = stringResource(R.string.ramdisk_editor_discard_archive_title),
                message = stringResource(R.string.ramdisk_editor_discard_archive_message),
                dismissLabel = stringResource(R.string.ramdisk_editor_cancel),
                confirmLabel = stringResource(R.string.ramdisk_editor_confirm),
                onDismiss = { discardPrompt = null },
                onDiscard = {
                    discardPrompt = null
                    navigator.popBackStack()
                },
            )
        }

        DiscardPrompt.TEXT_FILE -> {
            DiscardDialog(
                title = stringResource(R.string.ramdisk_editor_discard_text_title),
                message = stringResource(R.string.ramdisk_editor_discard_text_message),
                dismissLabel = stringResource(R.string.ramdisk_editor_continue_editing),
                confirmLabel = stringResource(R.string.ramdisk_editor_discard),
                onDismiss = { discardPrompt = null },
                onDiscard = {
                    discardPrompt = null
                    openedTextFile = null
                },
            )
        }

        null -> Unit
    }
}

@Composable
private fun RamdiskImageActions(
    dirty: Boolean,
    hasRebuiltImage: Boolean,
    isDumping: Boolean,
    onDump: () -> Unit,
    onExportImage: () -> Unit,
) {
    IconButton(
        enabled = hasRebuiltImage && !dirty && !isDumping,
        onClick = onExportImage,
    ) {
        YukiIcon(
            Icons.Filled.Download,
            contentDescription = stringResource(R.string.ramdisk_editor_export_image),
        )
    }
    IconButton(
        enabled = dirty && !isDumping,
        onClick = onDump,
    ) {
        if (isDumping) {
            CircularProgressIndicator(
                modifier = Modifier.size(22.dp),
                strokeWidth = 2.dp,
            )
        } else {
            YukiIcon(
                Icons.Filled.Save,
                contentDescription = stringResource(R.string.ramdisk_editor_rebuild_image),
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun RamdiskFragmentSelector(
    partitionName: String,
    fragments: List<YrcpRamdiskBackend>,
    dirty: Boolean,
    hasRebuiltImage: Boolean,
    isDumping: Boolean,
    snackbarHost: SnackbarHostState,
    onBack: () -> Unit,
    onSelect: (YrcpRamdiskBackend) -> Unit,
    onDump: () -> Unit,
    onExportImage: () -> Unit,
) {
    val title = @Composable {
        Text(stringResource(R.string.ramdisk_editor_fragments_title, partitionName))
    }
    val navigationIcon = @Composable {
        IconButton(onClick = onBack) {
            YukiIcon(
                Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = stringResource(R.string.ramdisk_editor_back),
            )
        }
    }
    val actions: @Composable RowScope.() -> Unit = {
        RamdiskImageActions(
            dirty = dirty,
            hasRebuiltImage = hasRebuiltImage,
            isDumping = isDumping,
            onDump = onDump,
            onExportImage = onExportImage,
        )
    }
    Scaffold(
        topBar = {
            if (isExpressiveUi) {
                LargeFlexibleTopAppBar(
                    title = title,
                    navigationIcon = navigationIcon,
                    actions = actions,
                )
            } else {
                TopAppBar(
                    title = title,
                    navigationIcon = navigationIcon,
                    actions = actions,
                )
            }
        },
        snackbarHost = { SnackbarHost(snackbarHost) },
    ) { paddingValues ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues),
            contentPadding = PaddingValues(
                horizontal = if (isExpressiveUi) 16.dp else 20.dp,
                vertical = 16.dp,
            ),
            verticalArrangement = Arrangement.spacedBy(if (isExpressiveUi) 8.dp else 12.dp),
        ) {
            item {
                Text(
                    text = stringResource(R.string.ramdisk_editor_fragments_description),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 4.dp, vertical = 4.dp),
                )
            }
            items(
                items = fragments,
                key = { it.fragment.index },
            ) { backend ->
                val fragment = backend.fragment
                Card(
                    onClick = { onSelect(backend) },
                    modifier = Modifier.fillMaxWidth(),
                    shape = if (isExpressiveUi) {
                        MaterialTheme.shapes.extraLarge
                    } else {
                        MaterialTheme.shapes.medium
                    },
                    colors = CardDefaults.cardColors(
                        containerColor = if (isExpressiveUi) {
                            MaterialTheme.colorScheme.secondaryContainer
                        } else {
                            MaterialTheme.colorScheme.surfaceContainer
                        },
                    ),
                ) {
                    ListItem(
                        content = {
                            Text(
                                text = fragment.name.ifBlank {
                                    stringResource(
                                        R.string.ramdisk_editor_fragment_fallback,
                                        fragment.index + 1,
                                    )
                                },
                                style = if (isExpressiveUi) {
                                    MaterialTheme.typography.titleLarge
                                } else {
                                    MaterialTheme.typography.titleMedium
                                },
                            )
                        },
                        supportingContent = {
                            Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                                Text(
                                    stringResource(
                                        R.string.ramdisk_editor_fragment_summary,
                                        fragment.vendorType.toVendorRamdiskType(),
                                        fragment.compression,
                                        Formatter.formatFileSize(
                                            LocalContext.current,
                                            fragment.packedSize,
                                        ),
                                    )
                                )
                                fragment.boardId
                                    .filter { it != 0L }
                                    .takeIf { it.isNotEmpty() }
                                    ?.let { boardId ->
                                        Text(
                                            text = stringResource(
                                                R.string.ramdisk_editor_fragment_board_id,
                                                boardId.joinToString(":") {
                                                    "%08x".format(Locale.US, it)
                                                },
                                            ),
                                            style = MaterialTheme.typography.bodySmall,
                                        )
                                    }
                            }
                        },
                        leadingContent = {
                            YukiIcon(
                                Icons.Filled.Memory,
                                contentDescription = null,
                                tint = if (isExpressiveUi) {
                                    MaterialTheme.colorScheme.onSecondaryContainer
                                } else {
                                    MaterialTheme.colorScheme.primary
                                },
                            )
                        },
                        colors = ListItemDefaults.colors(
                            containerColor = androidx.compose.ui.graphics.Color.Transparent
                        ),
                    )
                }
            }
        }
    }
}

@Composable
private fun Long.toVendorRamdiskType(): String =
    when (this) {
        1L -> stringResource(R.string.ramdisk_editor_fragment_type_platform)
        2L -> stringResource(R.string.ramdisk_editor_fragment_type_recovery)
        3L -> stringResource(R.string.ramdisk_editor_fragment_type_dlkm)
        else -> stringResource(R.string.ramdisk_editor_fragment_type_none)
    }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun RamdiskEditorStatusScreen(
    title: String,
    message: String,
    onBack: () -> Unit,
    loading: Boolean,
    onRetry: (() -> Unit)? = null,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(title) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        YukiIcon(
                            Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = stringResource(R.string.ramdisk_editor_back),
                        )
                    }
                },
            )
        },
    ) { paddingValues ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues)
                .padding(PaddingValues(24.dp)),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            if (loading) {
                CircularProgressIndicator()
            }
            Text(
                text = message,
                style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.padding(top = 16.dp),
            )
            if (onRetry != null) {
                Button(
                    onClick = onRetry,
                    modifier = Modifier.padding(top = 20.dp),
                ) {
                    YukiIcon(Icons.Filled.Refresh, contentDescription = null)
                    Text(
                        text = stringResource(R.string.ramdisk_editor_retry),
                        modifier = Modifier.padding(start = 8.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun DiscardDialog(
    title: String,
    message: String,
    dismissLabel: String,
    confirmLabel: String,
    onDismiss: () -> Unit,
    onDiscard: () -> Unit,
) {
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(message) },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(dismissLabel)
            }
        },
        confirmButton = {
            TextButton(onClick = onDiscard) {
                Text(
                    text = confirmLabel,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        },
    )
}

private suspend fun prepareRamdiskEditor(
    context: Context,
    partitionName: String,
    targetSlot: String?,
): RamdiskEditorLoadState.Ready {
    val directory = File(context.cacheDir, "ramdisk_editor").apply {
        check(isDirectory || mkdirs()) { "Cannot create the ramdisk editor cache directory" }
    }
    val sourceImage = File.createTempFile("source_", ".img", directory)
    val outputImage = File.createTempFile("rebuilt_", ".img", directory)
    val logs = mutableListOf<String>()
    try {
        val backedUp = PartitionManagerHelper.backupPartition(
            context = context,
            partition = partitionName,
            outputPath = sourceImage.absolutePath,
            slot = targetSlot,
            onStdout = logs::add,
            onStderr = { logs.add(it) },
        )
        if (!backedUp || sourceImage.length() <= 0L) {
            throw IOException(
                logs.lastOrNull()
                    ?: context.getString(R.string.ramdisk_editor_partition_backup_failed)
            )
        }
        val image = YrcpRamdiskImage.open(
            ksudPath = getKsud(),
            sourceImage = sourceImage,
            outputImage = outputImage,
            stagingDirectory = directory,
            rootDisplayName = context.getString(
                R.string.ramdisk_editor_root_name,
                partitionName,
            ),
        )
        return RamdiskEditorLoadState.Ready(
            image = image,
            sourceImage = sourceImage,
            outputImage = outputImage,
        )
    } catch (error: Throwable) {
        sourceImage.delete()
        outputImage.delete()
        throw error
    }
}

private fun queryDisplayName(context: Context, uri: Uri): String? =
    context.contentResolver.query(
        uri,
        arrayOf(OpenableColumns.DISPLAY_NAME),
        null,
        null,
        null,
    )?.use { cursor ->
        if (!cursor.moveToFirst()) return@use null
        val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        if (index < 0) null else cursor.getString(index)?.takeIf(String::isNotBlank)
    }

private fun readUtf8Text(input: java.io.InputStream): String {
    val output = ByteArrayOutputStream()
    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
    var total = 0
    while (true) {
        val count = input.read(buffer)
        if (count < 0) break
        total += count
        if (total > MAX_TEXT_FILE_SIZE) {
            throw IOException("The file exceeds the text editor limit")
        }
        output.write(buffer, 0, count)
    }
    val bytes = output.toByteArray()
    if (bytes.any { it == 0.toByte() }) {
        throw IOException("The selected file contains binary data")
    }
    return Charsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT)
        .decode(ByteBuffer.wrap(bytes))
        .toString()
}

private fun buildExportFileName(partitionName: String): String {
    val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
    return "${partitionName}_ramdisk_$timestamp.img"
}

private sealed interface RamdiskEditorLoadState {
    data object Loading : RamdiskEditorLoadState

    data class Ready(
        val image: YrcpRamdiskImage,
        val sourceImage: File,
        val outputImage: File,
    ) : RamdiskEditorLoadState

    data class Error(val message: String) : RamdiskEditorLoadState
}

private data class OpenedTextFile(
    val entry: FileEntry,
    val state: TextFileEditorState,
)

private enum class DiscardPrompt {
    ARCHIVE,
    TEXT_FILE,
}

private const val MAX_TEXT_FILE_SIZE = 2 * 1024 * 1024
private const val ROOT_EXIT_CONFIRM_INTERVAL_MILLIS = 2_500L
