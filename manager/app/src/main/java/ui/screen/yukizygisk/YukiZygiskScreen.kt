package ui.screen.yukizygisk

import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Article
import androidx.compose.material.icons.filled.Memory
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
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.util.execKsud
import com.anatdx.yukisu.ui.util.getRootShell
import com.anatdx.yukisu.ui.util.withNewRootShell
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

/** Mirrors uapi/yukizygisk.h yz_config + the yzconfig.json schema. */
data class YzConfig(
    val yukilinker: Boolean = false,
    val denylistMode: Int = 0, // 0=off, 1=force-umount+no-inject, 2=inject+umount
    val dmesgLog: Boolean = false,
)

private suspend fun readYzConfig(): YzConfig = withContext(Dispatchers.IO) {
    val raw = ShellUtils.fastCmd(getRootShell(), "cat $YZCONFIG_PATH 2>/dev/null")
    if (raw.isNullOrBlank()) return@withContext YzConfig()
    try {
        val o = JSONObject(raw)
        YzConfig(
            yukilinker = o.optBoolean("yukilinker", false),
            denylistMode = o.optInt("denylist_mode", 0),
            dmesgLog = o.optBoolean("dmesg_log", false),
        )
    } catch (_: Exception) {
        YzConfig()
    }
}

/** Write yzconfig.json then fire a netlink reload so it applies immediately. */
private suspend fun writeYzConfig(cfg: YzConfig) = withContext(Dispatchers.IO) {
    val json = JSONObject().apply {
        put("yukilinker", cfg.yukilinker)
        put("denylist_mode", cfg.denylistMode)
        put("dmesg_log", cfg.dmesgLog)
    }.toString(2)
    withNewRootShell {
        newJob().add("mkdir -p $YZCONFIG_DIR").exec()
        newJob().add("cat > $YZCONFIG_PATH <<'YZEOF'\n$json\nYZEOF").exec()
    }
    // Fires KSU_IOCTL_YZ_RELOAD -> kernel multicasts YZ_EV_RELOAD -> zygiskd
    // re-reads the file; takes effect on the next specialize, no reboot.
    execKsud("yukizygisk reload")
}

/** One injected app for the recent list: appid + resolved package identity. */
private data class RecentApp(
    val uid: Int,
    val label: String,             // "" when the package can't be resolved
    val packageName: String?,      // null when unresolved
    val packageInfo: PackageInfo?, // feeds Coil for the icon
)

/** Parsed view of zygiskd's status JSON (Natives.yzQueryStatus). */
private data class YzStatus(
    val count: Int,
    val recent: List<Int>, // appids, most-recent first
    val modules: List<String>,
)

private fun parseYzStatus(json: String): YzStatus? = runCatching {
    val o = JSONObject(json)
    val recent = o.optJSONArray("recent")?.let { a ->
        (0 until a.length()).map { a.getInt(it) }
    } ?: emptyList()
    val modules = o.optJSONArray("modules")?.let { a ->
        (0 until a.length()).map { a.getString(it) }
    } ?: emptyList()
    YzStatus(o.optInt("count", 0), recent, modules)
}.getOrNull()

/**
 * Resolve an injected appid to a displayable app. zygiskd reports appids
 * (uid % 100000); for the primary user that equals the uid, so user-0 apps
 * resolve directly and others fall back to the same package identity.
 */
private fun resolveRecentApp(pm: PackageManager, appId: Int): RecentApp {
    val pkg = runCatching { pm.getPackagesForUid(appId)?.firstOrNull() }.getOrNull()
        ?: return RecentApp(appId, "", null, null)
    val info = runCatching { pm.getPackageInfo(pkg, 0) }.getOrNull()
    val label = info?.applicationInfo?.loadLabel(pm)?.toString() ?: pkg
    return RecentApp(appId, label, pkg, info)
}

private const val YZ_POLL_INTERVAL_MS = 2000L

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun YukiZygiskScreen(navigator: DestinationsNavigator) {
    val scrollBehavior =
        TopAppBarDefaults.pinnedScrollBehavior(androidx.compose.material3.rememberTopAppBarState())
    val scope = rememberCoroutineScope()
    val snackBarHost = remember { SnackbarHostState() }

    val context = LocalContext.current
    var config by remember { mutableStateOf(YzConfig()) }
    var injectionActive by remember { mutableStateOf(false) }
    var injectionCount by remember { mutableIntStateOf(0) }
    var recentApps by remember { mutableStateOf<List<RecentApp>>(emptyList()) }
    var modulesLoadedCount by remember { mutableIntStateOf(0) }

    LaunchedEffect(Unit) {
        config = readYzConfig()
        injectionActive =
            ShellUtils.fastCmd(getRootShell(), "ksud feature get yukizygisk 2>/dev/null")
                ?.contains("enabled", ignoreCase = true) == true
    }

    // Injection telemetry: poll the running zygiskd in-process via Natives
    // (jni.c connects to the daemon socket; zygiskd SO_PEERCRED-gates the reply
    // to the kernel-authenticated manager uid). A non-null reply means injection
    // is live; null means the daemon is down or we are not the manager, so the
    // last-known values just stand.
    LaunchedEffect(Unit) {
        val pm = context.packageManager
        while (true) {
            val snapshot = withContext(Dispatchers.IO) {
                val json = runCatching { Natives.yzQueryStatus() }.getOrNull()
                    ?: return@withContext null
                val st = parseYzStatus(json) ?: return@withContext null
                Triple(st.count, st.recent.map { resolveRecentApp(pm, it) }, st.modules.size)
            }
            if (snapshot != null) {
                injectionActive = true
                injectionCount = snapshot.first
                recentApps = snapshot.second
                modulesLoadedCount = snapshot.third
            }
            delay(YZ_POLL_INTERVAL_MS)
        }
    }

    fun save(newCfg: YzConfig) {
        config = newCfg
        scope.launch { writeYzConfig(newCfg) }
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
            // --- Injection status ---
            SettingsCard(title = stringResource(R.string.yukizygisk_injection_status)) {
                StatusRow(
                    stringResource(R.string.yukizygisk_kernel_injection),
                    if (injectionActive) stringResource(R.string.yukizygisk_status_active)
                    else stringResource(R.string.yukizygisk_status_off),
                )
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

            // --- Recent injections ---
            SettingsCard(title = stringResource(R.string.yukizygisk_recent_injections)) {
                if (recentApps.isEmpty()) {
                    Text(
                        stringResource(R.string.yukizygisk_no_injections),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                    )
                } else {
                    recentApps.forEach { app -> RecentAppRow(app) }
                }
            }

            // --- Module loading ---
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

            // --- Denylist behaviour ---
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

            // --- Logging ---
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

/** One recent-injection row: app icon + label + "package · uid", like SuperUser. */
@Composable
private fun RecentAppRow(app: RecentApp) {
    val context = LocalContext.current
    ListItem(
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            AsyncImage(
                model = ImageRequest.Builder(context)
                    .data(app.packageInfo)
                    .crossfade(true)
                    .build(),
                contentDescription = null,
                modifier = Modifier.size(40.dp),
            )
        },
        headlineContent = {
            Text(
                if (app.label.isNotEmpty()) app.label
                else stringResource(R.string.yukizygisk_uid_fallback, app.uid),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        supportingContent = {
            Text(
                app.packageName?.let { "$it  ·  uid ${app.uid}" }
                    ?: stringResource(R.string.yukizygisk_uid_fallback, app.uid),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
    )
}

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
