/* SPDX-License-Identifier: GPL-3.0 */
/*
 * YukiZygisk manager panel.
 *
 * Author: Anatdx
 */
package ui.screen.yukizygisk

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Article
import androidx.compose.material.icons.filled.Adb
import androidx.compose.material.icons.filled.Extension
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material.icons.outlined.Cancel
import androidx.compose.material.icons.outlined.TaskAlt
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.util.execKsud
import com.anatdx.yukisu.ui.util.getRootShell
import com.anatdx.yukisu.ui.util.withNewRootShell
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.topjohnwu.superuser.ShellUtils
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import ui.screen.moreSettings.component.SettingsCard
import ui.screen.moreSettings.component.SwitchSettingItem

private const val YZCONFIG_DIR = "/data/adb/ksu/yukizygisk"
private const val YZCONFIG_PATH = "$YZCONFIG_DIR/yzconfig.json"

data class YzConfig(
    val yukilinker: Boolean = true,
    val denylistMode: Int = 0,
    val dmesgLog: Boolean = false,
)

private suspend fun readYzConfig(): YzConfig = withContext(Dispatchers.IO) {
    val raw = ShellUtils.fastCmd(getRootShell(), "cat $YZCONFIG_PATH 2>/dev/null")
    if (raw.isNullOrBlank()) return@withContext YzConfig()
    try {
        val o = JSONObject(raw)
        YzConfig(
            yukilinker = o.optBoolean("yukilinker", true),
            denylistMode = o.optInt("denylist_mode", 0),
            dmesgLog = o.optBoolean("dmesg_log", false),
        )
    } catch (_: Exception) {
        YzConfig()
    }
}

private suspend fun writeYzConfig(cfg: YzConfig) = withContext(Dispatchers.IO) {
    val json = JSONObject().apply {
        put("yukilinker", cfg.yukilinker)
        put("denylist_mode", cfg.denylistMode)
        put("dmesg_log", cfg.dmesgLog)
    }.toString()
    withNewRootShell {
        newJob().add("mkdir -p $YZCONFIG_DIR").exec()
        newJob().add("echo '$json' > $YZCONFIG_PATH").exec()
    }
    execKsud("yukizygisk reload")
}

private enum class MonitorState {
    Injected,
    Unsupported32,
    Crashed,
    Failed,
}

private enum class NativeMonitorMode {
    Module,
    Process,
}

private data class MonitorDialogState(
    val title: String,
    val message: String,
)

private fun parseMonitorState(value: String): MonitorState = when (value) {
    "unsupported32" -> MonitorState.Unsupported32
    "crashed" -> MonitorState.Crashed
    "failed" -> MonitorState.Failed
    else -> MonitorState.Injected
}

private fun mergeZygoteMonitorEntries(
    legacy: List<ZygoteMonitorEntry>,
    monitored: List<ZygoteMonitorEntry>,
): List<ZygoteMonitorEntry> {
    val merged = monitored.toMutableList()
    legacy.forEach { injected ->
        val idx = merged.indexOfFirst {
            (it.pid != 0 && it.pid == injected.pid) ||
                (it.name == injected.name && it.abi == injected.abi)
        }
        if (idx < 0) {
            merged += injected
        } else if (
            merged[idx].state != MonitorState.Unsupported32 &&
            merged[idx].state != MonitorState.Crashed
        ) {
            val current = merged[idx]
            merged[idx] = current.copy(
                pid = current.pid.takeIf { it != 0 } ?: injected.pid,
                name = current.name.ifEmpty { injected.name },
                abi = current.abi.takeUnless { it == "unknown" } ?: injected.abi,
                state = MonitorState.Injected,
            )
        }
    }
    return merged
}

private data class ZygoteMonitorEntry(
    val pid: Int,
    val name: String,
    val abi: String,
    val state: MonitorState,
)

private data class NativeModuleEntry(
    val id: String,
    val targetType: String,
    val target: String,
    val companion: Boolean,
    val state: MonitorState,
)

private data class NativeInjection(
    val pid: Int,
    val process: String,
    val module: String,
    val targetType: String,
    val target: String,
    val abi: String,
    val companion: Boolean,
    val state: MonitorState,
)

private data class NativeProcessEntry(
    val pid: Int,
    val process: String,
    val abi: String,
    val modules: List<String>,
    val state: MonitorState,
)

private data class NativeModuleMonitorEntry(
    val id: String,
    val targetType: String,
    val target: String,
    val companion: Boolean,
    val targets: List<NativeInjection>,
    val state: MonitorState,
)

private data class YzStatus(
    val count: Int,
    val safeMode: Boolean,
    val zygoteCrashes: Int,
    val zygotes: List<ZygoteMonitorEntry>,
    val modules: List<String>,
    val nativeModules: List<NativeModuleEntry>,
    val nativeInjections: List<NativeInjection>,
)

private fun parseYzStatus(json: String): YzStatus? = runCatching {
    val o = JSONObject(json)
    val modules = o.optJSONArray("modules")?.let { a ->
        (0 until a.length()).map { a.getString(it) }
    } ?: emptyList()
    val legacyZygotes = o.optJSONArray("zygotes")?.let { a ->
        (0 until a.length()).map { i ->
            val z = a.getJSONObject(i)
            ZygoteMonitorEntry(
                pid = z.optInt("pid", 0),
                name = z.optString("name", "zygote"),
                abi = z.optString("abi", "unknown"),
                state = MonitorState.Injected,
            )
        }
    } ?: emptyList()
    val monitoredZygoteArray = o.optJSONArray("zygote_monitor")
    val monitoredZygotes = monitoredZygoteArray?.let { a ->
        (0 until a.length()).map { i ->
            val z = a.getJSONObject(i)
            ZygoteMonitorEntry(
                pid = z.optInt("pid", 0),
                name = z.optString("name", "zygote"),
                abi = z.optString("abi", "unknown"),
                state = parseMonitorState(z.optString("state", "injected")),
            )
        }
    } ?: emptyList()
    val zygotes =
        if (monitoredZygoteArray != null && monitoredZygotes.isNotEmpty()) monitoredZygotes
        else mergeZygoteMonitorEntries(legacyZygotes, monitoredZygotes)
    val nativeModules = o.optJSONArray("native_modules")?.let { a ->
        (0 until a.length()).map { i ->
            val n = a.getJSONObject(i)
            NativeModuleEntry(
                id = n.optString("id", ""),
                targetType = n.optString("target_type", "name"),
                target = n.optString("target", ""),
                companion = n.optBoolean("companion", false),
                state = parseMonitorState(n.optString("state", "failed")),
            )
        }
    } ?: emptyList()
    val nativeInjections = o.optJSONArray("native_injections")?.let { a ->
        (0 until a.length()).map { i ->
            val n = a.getJSONObject(i)
            NativeInjection(
                pid = n.optInt("pid", 0),
                process = n.optString("process", ""),
                module = n.optString("module", ""),
                targetType = n.optString("target_type", "name"),
                target = n.optString("target", ""),
                abi = n.optString("abi", "unknown"),
                companion = n.optBoolean("companion", false),
                state = parseMonitorState(n.optString("state", "injected")),
            )
        }
    } ?: emptyList()
    YzStatus(
        o.optInt("count", 0),
        o.optBoolean("safe_mode", false),
        o.optInt("zygote_crashes", 0),
        zygotes,
        modules,
        nativeModules,
        nativeInjections,
    )
}.getOrNull()

private data class YzSnapshot(
    val count: Int,
    val safeMode: Boolean,
    val zygoteCrashes: Int,
    val zygotes: List<ZygoteMonitorEntry>,
    val modulesLoadedCount: Int,
    val nativeModules: List<NativeModuleEntry>,
    val nativeInjections: List<NativeInjection>,
)

private const val YZ_POLL_INTERVAL_MS = 2000L

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun YukiZygiskScreen(navigator: DestinationsNavigator) {
    val scrollBehavior =
        TopAppBarDefaults.pinnedScrollBehavior(androidx.compose.material3.rememberTopAppBarState())
    val scope = rememberCoroutineScope()
    val snackBarHost = remember { SnackbarHostState() }

    var config by remember { mutableStateOf(YzConfig()) }
    var injectionActive by remember { mutableStateOf(false) }
    var safeMode by remember { mutableStateOf(false) }
    var injectionCount by remember { mutableIntStateOf(0) }
    var zygoteCrashes by remember { mutableIntStateOf(0) }
    var monitoredZygotes by remember { mutableStateOf<List<ZygoteMonitorEntry>>(emptyList()) }
    var modulesLoadedCount by remember { mutableIntStateOf(0) }
    var nativeModules by remember { mutableStateOf<List<NativeModuleEntry>>(emptyList()) }
    var nativeInjections by remember { mutableStateOf<List<NativeInjection>>(emptyList()) }
    var nativeMonitorMode by remember { mutableStateOf(NativeMonitorMode.Module) }
    var monitorDialog by remember { mutableStateOf<MonitorDialogState?>(null) }

    LaunchedEffect(Unit) {
        config = readYzConfig()
        injectionActive =
            ShellUtils.fastCmd(getRootShell(), "ksud feature get yukizygisk 2>/dev/null")
                ?.contains("enabled", ignoreCase = true) == true
    }

    LaunchedEffect(Unit) {
        while (true) {
            val snapshot = withContext(Dispatchers.IO) {
                val json = runCatching { Natives.yzQueryStatus() }.getOrNull()
                    ?: return@withContext null
                val st = parseYzStatus(json) ?: return@withContext null
                YzSnapshot(
                    count = st.count,
                    safeMode = st.safeMode,
                    zygoteCrashes = st.zygoteCrashes,
                    zygotes = st.zygotes,
                    modulesLoadedCount = st.modules.size,
                    nativeModules = st.nativeModules,
                    nativeInjections = st.nativeInjections,
                )
            }
            if (snapshot != null) {
                safeMode = snapshot.safeMode
                injectionActive = !snapshot.safeMode
                injectionCount = snapshot.count
                zygoteCrashes = snapshot.zygoteCrashes
                monitoredZygotes = snapshot.zygotes
                modulesLoadedCount = snapshot.modulesLoadedCount
                nativeModules = snapshot.nativeModules
                nativeInjections = snapshot.nativeInjections
            }
            delay(YZ_POLL_INTERVAL_MS)
        }
    }

    fun save(newCfg: YzConfig) {
        config = newCfg
        scope.launch { writeYzConfig(newCfg) }
    }

    monitorDialog?.let { dialog ->
        AlertDialog(
            onDismissRequest = { monitorDialog = null },
            title = { Text(dialog.title) },
            text = { Text(dialog.message) },
            confirmButton = {
                TextButton(onClick = { monitorDialog = null }) {
                    Text(stringResource(R.string.close))
                }
            },
        )
    }

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        stringResource(R.string.settings_yukizygisk),
                        style = MaterialTheme.typography.titleLarge,
                    )
                },
                navigationIcon = {
                    IconButton(onClick = { navigator.popBackStack() }) {
                        Icon(
                            Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = stringResource(R.string.back),
                        )
                    }
                },
                windowInsets = WindowInsets.safeDrawing.only(
                    WindowInsetsSides.Top + WindowInsetsSides.Horizontal
                ),
                scrollBehavior = scrollBehavior,
            )
        },
        snackbarHost = { SnackbarHost(snackBarHost) },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        ),
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp)
                .padding(top = 8.dp),
        ) {
            SettingsCard(title = stringResource(R.string.yukizygisk_injection_status)) {
                StatusRow(
                    stringResource(R.string.yukizygisk_kernel_injection),
                    when {
                        safeMode -> stringResource(R.string.yukizygisk_status_safe_mode)
                        injectionActive -> stringResource(R.string.yukizygisk_status_active)
                        else -> stringResource(R.string.yukizygisk_status_off)
                    },
                )
                if (safeMode) {
                    StatusRow(
                        stringResource(R.string.yukizygisk_zygote_crashes),
                        zygoteCrashes.toString(),
                    )
                }
                StatusRow(
                    stringResource(R.string.yukizygisk_module_loader),
                    if (config.yukilinker) stringResource(R.string.yukizygisk_loader_anon)
                    else stringResource(R.string.yukizygisk_loader_system),
                )
                StatusRow(
                    stringResource(R.string.yukizygisk_denylist_behaviour),
                    when (config.denylistMode) {
                        1 -> stringResource(R.string.yukizygisk_denylist_force_long)
                        2 -> stringResource(R.string.yukizygisk_denylist_restore_long)
                        else -> stringResource(R.string.yukizygisk_status_off)
                    },
                )
                StatusRow(
                    stringResource(R.string.yukizygisk_injections_session),
                    injectionCount.toString(),
                )
            }

            SettingsCard(title = stringResource(R.string.yukizygisk_injected_zygotes)) {
                if (monitoredZygotes.isEmpty()) {
                    Text(
                        stringResource(R.string.yukizygisk_no_zygotes),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                    )
                } else {
                    monitoredZygotes.forEach { zygote ->
                        val dialog = zygoteDialog(zygote)
                        ZygoteMonitorRow(zygote) {
                            monitorDialog = dialog
                        }
                    }
                }
            }

            MonitorCard(
                title = stringResource(R.string.yukizygisk_native_injections),
                trailing = {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.clickable {
                            nativeMonitorMode = when (nativeMonitorMode) {
                                NativeMonitorMode.Module -> NativeMonitorMode.Process
                                NativeMonitorMode.Process -> NativeMonitorMode.Module
                            }
                        },
                    ) {
                        Text(
                            when (nativeMonitorMode) {
                                NativeMonitorMode.Module ->
                                    stringResource(R.string.yukizygisk_native_mode_module)
                                NativeMonitorMode.Process ->
                                    stringResource(R.string.yukizygisk_native_mode_process)
                            },
                            style = MaterialTheme.typography.bodySmall,
                            fontWeight = FontWeight.Light,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        IconButton(
                            onClick = {
                                nativeMonitorMode = when (nativeMonitorMode) {
                                    NativeMonitorMode.Module -> NativeMonitorMode.Process
                                    NativeMonitorMode.Process -> NativeMonitorMode.Module
                                }
                            },
                            modifier = Modifier.size(36.dp),
                        ) {
                            Icon(
                                imageVector = Icons.Filled.SwapHoriz,
                                contentDescription = null,
                                modifier = Modifier.size(20.dp),
                            )
                        }
                    }
                },
            ) {
                val moduleRows = remember(nativeModules, nativeInjections) {
                    nativeModules.map { module ->
                        val targets = nativeInjections.filter { it.module == module.id }
                        val state = when {
                            targets.any { it.state == MonitorState.Crashed } -> MonitorState.Crashed
                            targets.any { it.state == MonitorState.Failed } -> MonitorState.Failed
                            targets.any { it.state == MonitorState.Unsupported32 } ->
                                MonitorState.Unsupported32
                            targets.any { it.state == MonitorState.Injected } -> MonitorState.Injected
                            else -> module.state
                        }
                        NativeModuleMonitorEntry(
                            id = module.id,
                            targetType = module.targetType,
                            target = module.target,
                            companion = module.companion,
                            targets = targets,
                            state = state,
                        )
                    }
                }
                val processRows = remember(nativeInjections) {
                    nativeInjections
                        .groupBy { it.pid to it.process }
                        .map { (_, rows) ->
                            val first = rows.first()
                            val state = when {
                                rows.any { it.state == MonitorState.Crashed } -> MonitorState.Crashed
                                rows.any { it.state == MonitorState.Failed } -> MonitorState.Failed
                                rows.any { it.state == MonitorState.Unsupported32 } ->
                                    MonitorState.Unsupported32
                                else -> MonitorState.Injected
                            }
                            NativeProcessEntry(
                                pid = first.pid,
                                process = first.process.ifEmpty { first.target },
                                abi = first.abi,
                                modules = rows.map { it.module }.distinct(),
                                state = state,
                            )
                        }
                        .sortedWith(compareBy<NativeProcessEntry> { it.process }.thenBy { it.pid })
                }

                if (nativeMonitorMode == NativeMonitorMode.Module && moduleRows.isEmpty()) {
                    Text(
                        stringResource(R.string.yukizygisk_no_native_modules),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                    )
                } else if (nativeMonitorMode == NativeMonitorMode.Process && processRows.isEmpty()) {
                    Text(
                        stringResource(R.string.yukizygisk_no_native_injections),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                    )
                } else if (nativeMonitorMode == NativeMonitorMode.Module) {
                    moduleRows.forEach { module ->
                        val dialog = nativeModuleDialog(module)
                        NativeModuleMonitorRow(module) {
                            monitorDialog = dialog
                        }
                    }
                } else {
                    processRows.forEach { process ->
                        val dialog = nativeProcessDialog(process)
                        NativeProcessMonitorRow(process) {
                            monitorDialog = dialog
                        }
                    }
                }
            }

            SettingsCard(title = stringResource(R.string.yukizygisk_module_loading)) {
                SwitchSettingItem(
                    icon = Icons.Filled.Memory,
                    title = stringResource(R.string.yukizygisk_anon_loading_title),
                    summary = stringResource(R.string.yukizygisk_anon_loading_summary),
                    checked = config.yukilinker,
                    onChange = { save(config.copy(yukilinker = it)) },
                )
                Text(
                    stringResource(R.string.yukizygisk_loaded_modules_count, modulesLoadedCount),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp)
                        .padding(top = 4.dp, bottom = 4.dp),
                )
            }

            SettingsCard(title = stringResource(R.string.yukizygisk_denylist_behaviour)) {
                Text(
                    stringResource(R.string.yukizygisk_denylist_desc),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .padding(horizontal = 16.dp)
                        .padding(bottom = 8.dp),
                )
                DenylistModeSelector(
                    mode = config.denylistMode,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp),
                ) { save(config.copy(denylistMode = it)) }
            }

            SettingsCard(title = stringResource(R.string.yukizygisk_log_dmesg_title)) {
                SwitchSettingItem(
                    icon = Icons.AutoMirrored.Filled.Article,
                    title = stringResource(R.string.yukizygisk_log_dmesg_title),
                    summary = stringResource(R.string.yukizygisk_log_dmesg_summary),
                    checked = config.dmesgLog,
                    onChange = { save(config.copy(dmesgLog = it)) },
                )
            }
        }
    }
}

@Composable
private fun ZygoteMonitorRow(zygote: ZygoteMonitorEntry, onStatusClick: () -> Unit) {
    ListItem(
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            Icon(
                imageVector = Icons.Filled.Adb,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(28.dp),
            )
        },
        headlineContent = {
            Text(
                zygote.name,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        supportingContent = {
            Text(
                stringResource(
                    R.string.yukizygisk_zygote_detail,
                    zygote.abi,
                    zygote.pid,
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        trailingContent = {
            MonitorStateButton(zygote.state, onStatusClick)
        },
    )
}

@Composable
private fun NativeModuleMonitorRow(module: NativeModuleMonitorEntry, onStatusClick: () -> Unit) {
    ListItem(
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            Icon(
                imageVector = Icons.Filled.Extension,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(28.dp),
            )
        },
        headlineContent = {
            Text(
                module.id,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        supportingContent = {
            Text(
                stringResource(
                    R.string.yukizygisk_native_module_detail,
                    module.targetType,
                    module.target,
                    if (module.companion) stringResource(R.string.yukizygisk_native_companion)
                    else stringResource(R.string.yukizygisk_native_no_companion),
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        trailingContent = {
            MonitorStateButton(module.state, onStatusClick)
        },
    )
}

@Composable
private fun NativeProcessMonitorRow(process: NativeProcessEntry, onStatusClick: () -> Unit) {
    ListItem(
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            Icon(
                imageVector = Icons.Filled.Terminal,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(28.dp),
            )
        },
        headlineContent = {
            Text(
                process.process,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        supportingContent = {
            Text(
                stringResource(
                    R.string.yukizygisk_native_process_detail,
                    process.abi,
                    process.pid,
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        trailingContent = {
            MonitorStateButton(process.state, onStatusClick)
        },
    )
}

@Composable
private fun MonitorStateButton(state: MonitorState, onClick: () -> Unit) {
    val icon: ImageVector
    val tint: Color
    when (state) {
        MonitorState.Injected -> {
            icon = Icons.Outlined.TaskAlt
            tint = MaterialTheme.colorScheme.primary
        }
        MonitorState.Unsupported32 -> {
            icon = Icons.Outlined.Warning
            tint = MaterialTheme.colorScheme.tertiary
        }
        MonitorState.Crashed -> {
            icon = Icons.Outlined.Warning
            tint = MaterialTheme.colorScheme.error
        }
        MonitorState.Failed -> {
            icon = Icons.Outlined.Cancel
            tint = MaterialTheme.colorScheme.error
        }
    }
    IconButton(
        onClick = onClick,
        modifier = Modifier
            .padding(start = 8.dp)
            .size(40.dp),
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = tint,
            modifier = Modifier.size(22.dp),
        )
    }
}

@Composable
private fun MonitorCard(
    title: String,
    trailing: @Composable (() -> Unit)? = null,
    content: @Composable () -> Unit,
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(bottom = 16.dp),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerHigh),
        elevation = getCardElevation(),
        shape = MaterialTheme.shapes.medium,
    ) {
        Column(modifier = Modifier.padding(vertical = 8.dp)) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(48.dp)
                    .padding(start = 16.dp, end = 8.dp),
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.weight(1f),
                )
                trailing?.invoke()
            }
            content()
        }
    }
}

@Composable
private fun zygoteDialog(zygote: ZygoteMonitorEntry): MonitorDialogState {
    val message = when (zygote.state) {
        MonitorState.Injected -> stringResource(R.string.yukizygisk_zygote_injected_message)
        MonitorState.Unsupported32 -> stringResource(R.string.yukizygisk_zygote_unsupported_message)
        MonitorState.Crashed -> stringResource(R.string.yukizygisk_zygote_crashed_message)
        MonitorState.Failed -> stringResource(R.string.yukizygisk_zygote_failed_message)
    }
    return MonitorDialogState(zygote.name, message)
}

@Composable
private fun nativeProcessDialog(process: NativeProcessEntry): MonitorDialogState {
    val context = LocalContext.current
    val modules = process.modules.joinToString("\n") {
        context.getString(R.string.yukizygisk_native_process_module_line, it)
    }
    val base = when (process.state) {
        MonitorState.Injected -> stringResource(R.string.yukizygisk_native_process_injected_message)
        MonitorState.Unsupported32 ->
            stringResource(R.string.yukizygisk_native_process_unsupported_message)
        MonitorState.Crashed -> stringResource(R.string.yukizygisk_native_process_failed_message)
        MonitorState.Failed -> stringResource(R.string.yukizygisk_native_process_failed_message)
    }
    return MonitorDialogState(process.process, appendDetail(base, modules))
}

@Composable
private fun nativeModuleDialog(module: NativeModuleMonitorEntry): MonitorDialogState {
    val context = LocalContext.current
    val targets = module.targets.joinToString("\n") {
        context.getString(
            R.string.yukizygisk_native_target_line,
            it.process.ifEmpty { it.target },
            it.abi,
            it.pid,
        )
    }
    val base = when (module.state) {
        MonitorState.Injected -> stringResource(R.string.yukizygisk_native_module_injected_message)
        MonitorState.Unsupported32 ->
            stringResource(R.string.yukizygisk_native_module_unsupported_message)
        MonitorState.Crashed -> stringResource(R.string.yukizygisk_native_module_failed_message)
        MonitorState.Failed -> stringResource(R.string.yukizygisk_native_module_failed_message)
    }
    return MonitorDialogState(module.id, appendDetail(base, targets))
}

private fun appendDetail(base: String, detail: String): String =
    if (detail.isBlank()) base else "$base\n$detail"

@Composable
private fun StatusRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            label,
            style = MaterialTheme.typography.bodyLarge,
            modifier = Modifier.weight(1f, fill = false),
        )
        Text(
            value,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.primary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(start = 12.dp),
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DenylistModeSelector(
    mode: Int,
    modifier: Modifier = Modifier,
    onSelect: (Int) -> Unit,
) {
    val options = listOf(
        stringResource(R.string.yukizygisk_denylist_off),
        stringResource(R.string.yukizygisk_denylist_force),
        stringResource(R.string.yukizygisk_denylist_restore),
    )
    SingleChoiceSegmentedButtonRow(modifier = modifier) {
        options.forEachIndexed { index, label ->
            SegmentedButton(
                selected = mode == index,
                onClick = { onSelect(index) },
                shape = SegmentedButtonDefaults.itemShape(index, options.size),
            ) {
                Text(label)
            }
        }
    }
}
