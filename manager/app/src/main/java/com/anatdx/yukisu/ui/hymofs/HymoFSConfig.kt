package com.anatdx.yukisu.ui.hymofs

import android.annotation.SuppressLint
import android.content.Context
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.hymofs.util.HymoFSManager
import com.anatdx.yukisu.ui.hymofs.util.HymoFSManager.HymoFSStatus
import com.anatdx.yukisu.ui.util.getSupportedKmis
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.launch

/**
 * Tab enum for HymoFS config screen
 */
enum class HymoFSTab(val displayNameRes: Int) {
    STATUS(R.string.hymofs_tab_status),
    LKM(R.string.hymofs_tab_lkm),
    SETTINGS(R.string.hymofs_tab_settings),
    RULES(R.string.hymofs_tab_rules),
    LOGS(R.string.hymofs_tab_logs)
}

/**
 * HymoFS Configuration Screen
 */
@SuppressLint("SdCardPath")
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun HymoFSConfigScreen(
    navigator: DestinationsNavigator
) {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    val snackbarHostState = remember { SnackbarHostState() }

    // State
    var selectedTab by remember { mutableStateOf(HymoFSTab.STATUS) }
    var isLoading by remember { mutableStateOf(true) }
    
    // Data
    var hymofsStatus by remember { mutableStateOf(HymoFSStatus.NOT_PRESENT) }
    var version by remember { mutableStateOf("Unknown") }
    var config by remember { mutableStateOf(HymoFSManager.HymoConfig()) }
    var modules by remember { mutableStateOf(emptyList<HymoFSManager.ModuleInfo>()) }
    var activeRules by remember { mutableStateOf(emptyList<HymoFSManager.ActiveRule>()) }
    var systemInfo by remember { mutableStateOf(HymoFSManager.SystemInfo("", "", "", emptyList(), emptyList(), false, null)) }
    var storageInfo by remember { mutableStateOf(HymoFSManager.StorageInfo("-", "-", "-", "0%", "unknown")) }
    var features by remember { mutableStateOf<HymoFSManager.FeaturesResult?>(null) }
    var logContent by remember { mutableStateOf("") }
    var showKernelLog by remember { mutableStateOf(false) }

    // Load data
    fun loadData() {
        coroutineScope.launch {
            isLoading = true
            try {
                version = HymoFSManager.getVersion()
                hymofsStatus = HymoFSManager.getStatus()
                config = HymoFSManager.loadConfig()
                modules = HymoFSManager.getModules()
                systemInfo = HymoFSManager.getSystemInfo()
                storageInfo = HymoFSManager.getStorageInfo()
                if (hymofsStatus == HymoFSStatus.AVAILABLE) {
                    activeRules = HymoFSManager.getActiveRules()
                    features = HymoFSManager.getFeatures()
                } else {
                    features = null
                }
            } catch (e: Exception) {
                val msg = context.getString(
                    R.string.hymofs_toast_load_error,
                    e.message ?: "unknown"
                )
                snackbarHostState.showSnackbar(msg)
            }
            isLoading = false
        }
    }

    // Initial load
    LaunchedEffect(Unit) {
        loadData()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        text = stringResource(R.string.hymofs_title),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                },
                navigationIcon = {
                    IconButton(onClick = { navigator.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null)
                    }
                },
                actions = {
                    IconButton(onClick = { loadData() }) {
                        Icon(Icons.Filled.Refresh, contentDescription = "Refresh")
                    }
                }
            )
        },
        snackbarHost = { SnackbarHost(snackbarHostState) }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            // Tab row
            ScrollableTabRow(
                selectedTabIndex = HymoFSTab.entries.indexOf(selectedTab),
                edgePadding = 16.dp
            ) {
                HymoFSTab.entries.forEach { tab ->
                    Tab(
                        selected = selectedTab == tab,
                        onClick = { selectedTab = tab },
                        text = { Text(stringResource(tab.displayNameRes)) }
                    )
                }
            }

            if (isLoading) {
                Box(
                    modifier = Modifier.fillMaxSize(),
                    contentAlignment = Alignment.Center
                ) {
                    CircularProgressIndicator()
                }
            } else {
                when (selectedTab) {
                    HymoFSTab.STATUS -> StatusTab(
                        hymofsStatus = hymofsStatus,
                        hymofsBuiltin = config.hymofsBuiltin,
                        version = version,
                        systemInfo = systemInfo,
                        storageInfo = storageInfo,
                        modules = modules,
                        features = features,
                        onRefresh = { loadData() }
                    )
                    HymoFSTab.LKM -> LkmTab(
                        hymofsStatus = hymofsStatus,
                        version = version,
                        systemInfo = systemInfo,
                        config = config,
                        onRefresh = { loadData() },
                        snackbarHostState = snackbarHostState,
                        onLkmAutoloadChanged = { enable ->
                            coroutineScope.launch {
                                if (HymoFSManager.setLkmAutoload(enable)) {
                                    config = config.copy(lkmAutoload = enable)
                                    snackbarHostState.showSnackbar(
                                        context.getString(
                                            if (enable) R.string.hymofs_lkm_autoload_enabled
                                            else R.string.hymofs_lkm_autoload_disabled
                                        )
                                    )
                                } else {
                                    snackbarHostState.showSnackbar(
                                        context.getString(R.string.hymofs_lkm_autoload_failed)
                                    )
                                }
                            }
                        }
                    )
                    HymoFSTab.SETTINGS -> SettingsTab(
                        config = config,
                        hymofsStatus = hymofsStatus,
                        features = features,
                        snackbarHostState = snackbarHostState,
                        onConfigChanged = { newConfig ->
                            coroutineScope.launch {
                                if (HymoFSManager.saveConfig(newConfig)) {
                                    config = newConfig
                                    snackbarHostState.showSnackbar(
                                        context.getString(R.string.hymofs_toast_settings_saved)
                                    )
                                } else {
                                    snackbarHostState.showSnackbar(
                                        context.getString(R.string.hymofs_toast_settings_failed)
                                    )
                                }
                            }
                        },
                        onSetDebug = { enable ->
                            coroutineScope.launch {
                                if (HymoFSManager.setKernelDebug(enable)) {
                                    val msgRes = if (enable) {
                                        R.string.hymofs_toast_kernel_debug_enabled
                                    } else {
                                        R.string.hymofs_toast_kernel_debug_disabled
                                    }
                                    snackbarHostState.showSnackbar(context.getString(msgRes))
                                }
                            }
                        },
                        onSetStealth = { enable ->
                            coroutineScope.launch {
                                if (HymoFSManager.setStealth(enable)) {
                                    val msgRes = if (enable) {
                                        R.string.hymofs_toast_stealth_enabled
                                    } else {
                                        R.string.hymofs_toast_stealth_disabled
                                    }
                                    snackbarHostState.showSnackbar(context.getString(msgRes))
                                }
                            }
                        },
                        onFixMounts = {
                            coroutineScope.launch {
                                if (HymoFSManager.fixMounts()) {
                                    snackbarHostState.showSnackbar(
                                        context.getString(R.string.hymofs_toast_mounts_fixed)
                                    )
                                } else {
                                    snackbarHostState.showSnackbar(
                                        context.getString(R.string.hymofs_toast_mounts_failed)
                                    )
                                }
                            }
                        }
                    )
                    HymoFSTab.RULES -> RulesTab(
                        activeRules = activeRules,
                        hymofsStatus = hymofsStatus,
                        onRefresh = {
                            coroutineScope.launch {
                                activeRules = HymoFSManager.getActiveRules()
                            }
                        },
                        onClearAll = {
                            coroutineScope.launch {
                                if (HymoFSManager.clearAllRules()) {
                                    activeRules = emptyList()
                                    snackbarHostState.showSnackbar("All rules cleared")
                                } else {
                                    snackbarHostState.showSnackbar("Failed to clear rules")
                                }
                            }
                        }
                    )
                    HymoFSTab.LOGS -> LogsTab(
                        showKernelLog = showKernelLog,
                        onToggleLogType = { showKernelLog = !showKernelLog },
                        logContent = logContent,
                        onRefreshLog = {
                            coroutineScope.launch {
                                logContent = if (showKernelLog) {
                                    HymoFSManager.readKernelLog()
                                } else {
                                    HymoFSManager.readLog()
                                }
                            }
                        }
                    )
                }
            }
        }
    }
}

// ==================== Status Tab ====================
@Composable
private fun StatusTab(
    hymofsStatus: HymoFSStatus,
    hymofsBuiltin: Boolean,
    version: String,
    systemInfo: HymoFSManager.SystemInfo,
    storageInfo: HymoFSManager.StorageInfo,
    modules: List<HymoFSManager.ModuleInfo>,
    features: HymoFSManager.FeaturesResult?,
    onRefresh: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        // HymoFS Status Card
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            colors = CardDefaults.cardColors(
                containerColor = when (hymofsStatus) {
                    HymoFSStatus.AVAILABLE -> Color(0xFF1B5E20).copy(alpha = 0.2f)
                    HymoFSStatus.NOT_PRESENT -> MaterialTheme.colorScheme.surfaceVariant
                    else -> Color(0xFFE65100).copy(alpha = 0.2f)
                }
            )
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Icon(
                        imageVector = when (hymofsStatus) {
                            HymoFSStatus.AVAILABLE -> Icons.Filled.CheckCircle
                            HymoFSStatus.NOT_PRESENT -> Icons.Filled.Info
                            else -> Icons.Filled.Warning
                        },
                        contentDescription = null,
                        tint = when (hymofsStatus) {
                            HymoFSStatus.AVAILABLE -> Color(0xFF4CAF50)
                            HymoFSStatus.NOT_PRESENT -> MaterialTheme.colorScheme.onSurfaceVariant
                            else -> Color(0xFFFF9800)
                        },
                        modifier = Modifier.size(32.dp)
                    )
                    Column {
                        Text(
                            text = stringResource(R.string.hymofs_kernel_title),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            text = stringResource(
                                when {
                                    hymofsStatus == HymoFSStatus.AVAILABLE && hymofsBuiltin ->
                                        R.string.hymofs_status_builtin
                                    hymofsStatus == HymoFSStatus.AVAILABLE ->
                                        R.string.hymofs_status_available
                                    hymofsStatus == HymoFSStatus.NOT_PRESENT ->
                                        R.string.hymofs_status_not_present
                                    hymofsStatus == HymoFSStatus.KERNEL_TOO_OLD ->
                                        R.string.hymofs_status_kernel_too_old
                                    else -> R.string.hymofs_status_module_too_old
                                }
                            ),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                
                if (hymofsStatus == HymoFSStatus.AVAILABLE) {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = stringResource(R.string.hymofs_version_label, version),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }

        // Kernel features (when available): list each with short description so Maps/Statfs are visible
        if (hymofsStatus == HymoFSStatus.AVAILABLE) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
                elevation = getCardElevation()
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(
                        text = stringResource(R.string.hymofs_features_title),
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.padding(bottom = 12.dp)
                    )
                    val names = features?.names?.toSet() ?: emptySet()
                    if (names.isEmpty()) {
                        Text(
                            text = stringResource(R.string.hymofs_features_none),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    } else {
                        if (names.contains("mount_hide")) {
                            FeatureRow(
                                title = stringResource(R.string.hymofs_feature_mount_hide),
                                desc = stringResource(R.string.hymofs_feature_mount_hide_desc)
                            )
                        }
                        if (names.contains("maps_spoof")) {
                            FeatureRow(
                                title = stringResource(R.string.hymofs_feature_maps_spoof),
                                desc = stringResource(R.string.hymofs_feature_maps_spoof_desc)
                            )
                        }
                        if (names.contains("statfs_spoof")) {
                            FeatureRow(
                                title = stringResource(R.string.hymofs_feature_statfs_spoof),
                                desc = stringResource(R.string.hymofs_feature_statfs_spoof_desc)
                            )
                        }
                        val other = names - setOf("mount_hide", "maps_spoof", "statfs_spoof")
                        if (other.isNotEmpty()) {
                            Text(
                                text = other.sorted().joinToString(", "),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.padding(top = 4.dp)
                            )
                        }
                    }
                }
            }
        }
        
        // Storage Card
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation()
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = stringResource(R.string.hymofs_storage),
                        style = MaterialTheme.typography.titleMedium
                    )
                    if (storageInfo.type != "unknown") {
                        Surface(
                            shape = RoundedCornerShape(4.dp),
                            color = if (storageInfo.type == "tmpfs" || storageInfo.type == "hymofs")
                                MaterialTheme.colorScheme.primaryContainer
                            else
                                MaterialTheme.colorScheme.secondaryContainer
                        ) {
                            Text(
                                text = storageInfo.type.uppercase(),
                                modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                                style = MaterialTheme.typography.labelSmall,
                                fontWeight = FontWeight.Bold
                            )
                        }
                    }
                }
                
                Spacer(modifier = Modifier.height(12.dp))
                
                LinearProgressIndicator(
                    progress = { storageInfo.percent.removeSuffix("%").toFloatOrNull()?.div(100) ?: 0f },
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(8.dp),
                    trackColor = MaterialTheme.colorScheme.surfaceVariant
                )
                
                Spacer(modifier = Modifier.height(8.dp))
                
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = systemInfo.mountBase,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Text(
                        text = "${storageInfo.used} / ${storageInfo.size}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
        
        // Stats Row
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            StatCard(
                modifier = Modifier.weight(1f),
                value = modules.size.toString(),
                label = stringResource(R.string.hymofs_modules_count)
            )
            StatCard(
                modifier = Modifier.weight(1f),
                value = if (hymofsStatus == HymoFSStatus.AVAILABLE)
                    systemInfo.hymofsModuleIds.size.toString()
                else "❌",
                label = stringResource(R.string.hymofs_stats_hymofs)
            )
        }
        
        // System Info Card
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation()
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text(
                    text = stringResource(R.string.hymofs_system_info),
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(bottom = 12.dp)
                )
                
                InfoRow(label = stringResource(R.string.hymofs_info_kernel), value = systemInfo.kernel)
                InfoRow(label = stringResource(R.string.hymofs_info_selinux), value = systemInfo.selinux)
                InfoRow(label = stringResource(R.string.hymofs_info_mount_base), value = systemInfo.mountBase)
                
                if (systemInfo.activeMounts.isNotEmpty()) {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = stringResource(R.string.hymofs_info_active_mounts),
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    systemInfo.activeMounts.take(5).forEach { mount ->
                        Text(
                            text = "  • $mount",
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                    if (systemInfo.activeMounts.size > 5) {
                        Text(
                            text = stringResource(R.string.hymofs_info_more_mounts, systemInfo.activeMounts.size - 5),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }
        
        // Mount Statistics (aligned with WebUI StatusPage)
        systemInfo.mountStats?.let { ms ->
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
                elevation = getCardElevation()
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(
                        text = "Mount Statistics",
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.padding(bottom = 12.dp)
                    )
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        StatCard(modifier = Modifier.weight(1f), value = ms.totalMounts.toString(), label = "Total")
                        StatCard(modifier = Modifier.weight(1f), value = ms.successfulMounts.toString(), label = "Success")
                        StatCard(modifier = Modifier.weight(1f), value = ms.failedMounts.toString(), label = "Failed")
                        StatCard(
                            modifier = Modifier.weight(1f),
                            value = ms.successRate?.let { "${it.toInt()}%" } ?: "N/A",
                            label = "Rate"
                        )
                    }
                    Spacer(modifier = Modifier.height(8.dp))
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Text("Files: ${ms.filesMounted}", style = MaterialTheme.typography.bodySmall)
                        Text("Dirs: ${ms.dirsMounted}", style = MaterialTheme.typography.bodySmall)
                        Text("Symlinks: ${ms.symlinksCreated}", style = MaterialTheme.typography.bodySmall)
                        Text("Overlay: ${ms.overlayfsMounts}", style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }
        
        // Partitions (aligned with WebUI)
        if (systemInfo.detectedPartitions.isNotEmpty()) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
                elevation = getCardElevation()
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(
                        text = "Partitions",
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.padding(bottom = 8.dp)
                    )
                    LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        items(systemInfo.detectedPartitions) { p ->
                            AssistChip(
                                onClick = { },
                                label = { Text(p.name) },
                                modifier = Modifier.height(32.dp)
                            )
                        }
                    }
                }
            }
        }
        
        // Warning for mismatch
        if (systemInfo.hymofsMismatch) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = CardDefaults.cardColors(
                    containerColor = Color(0xFFE65100).copy(alpha = 0.2f)
                )
            ) {
                Row(
                    modifier = Modifier.padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        Icons.Filled.Warning,
                        contentDescription = null,
                        tint = Color(0xFFFF9800)
                    )
                    Text(
                        text = systemInfo.mismatchMessage ?: stringResource(R.string.hymofs_mismatch_default),
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }
        }
    }
}

@Composable
private fun StatCard(
    modifier: Modifier = Modifier,
    value: String,
    label: String
) {
    Card(
        modifier = modifier,
        shape = RoundedCornerShape(16.dp),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerHigh),
        elevation = getCardElevation()
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = value,
                style = MaterialTheme.typography.headlineMedium,
                fontWeight = FontWeight.Bold
            )
            Text(
                text = label,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontFamily = FontFamily.Monospace
        )
    }
}

@Composable
private fun FeatureRow(title: String, desc: String) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 6.dp)
    ) {
        Text(
            text = title,
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.primary
        )
        Text(
            text = desc,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

// ==================== LKM Tab ====================
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LkmTab(
    hymofsStatus: HymoFSStatus,
    version: String,
    systemInfo: HymoFSManager.SystemInfo,
    config: HymoFSManager.HymoConfig,
    onRefresh: () -> Unit,
    snackbarHostState: SnackbarHostState,
    onLkmAutoloadChanged: (Boolean) -> Unit
) {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        // HymoFS LKM status card
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            colors = CardDefaults.cardColors(
                containerColor = when (hymofsStatus) {
                    HymoFSStatus.AVAILABLE -> Color(0xFF1B5E20).copy(alpha = 0.2f)
                    HymoFSStatus.NOT_PRESENT -> MaterialTheme.colorScheme.surfaceVariant
                    else -> Color(0xFFE65100).copy(alpha = 0.2f)
                }
            )
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Icon(
                        imageVector = when (hymofsStatus) {
                            HymoFSStatus.AVAILABLE -> Icons.Filled.CheckCircle
                            HymoFSStatus.NOT_PRESENT -> Icons.Filled.Info
                            else -> Icons.Filled.Warning
                        },
                        contentDescription = null,
                        tint = when (hymofsStatus) {
                            HymoFSStatus.AVAILABLE -> Color(0xFF4CAF50)
                            HymoFSStatus.NOT_PRESENT -> MaterialTheme.colorScheme.onSurfaceVariant
                            else -> Color(0xFFFF9800)
                        },
                        modifier = Modifier.size(32.dp)
                    )
                    Column {
                        Text(
                            text = stringResource(R.string.hymofs_lkm_card_title),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            text = stringResource(
                                when {
                                    hymofsStatus == HymoFSStatus.AVAILABLE && config.hymofsBuiltin ->
                                        R.string.hymofs_status_builtin
                                    hymofsStatus == HymoFSStatus.AVAILABLE ->
                                        R.string.hymofs_status_available
                                    hymofsStatus == HymoFSStatus.NOT_PRESENT ->
                                        R.string.hymofs_status_not_present
                                    hymofsStatus == HymoFSStatus.KERNEL_TOO_OLD ->
                                        R.string.hymofs_status_kernel_too_old
                                    else -> R.string.hymofs_status_module_too_old
                                }
                            ),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                if (hymofsStatus == HymoFSStatus.AVAILABLE) {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = stringResource(R.string.hymofs_version_label, version),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                Spacer(modifier = Modifier.height(12.dp))
                InfoRow(
                    stringResource(R.string.hymofs_lkm_kernel_label),
                    systemInfo.kernel.ifEmpty { "-" }
                )
                if (config.unameRelease.isNotBlank()) {
                    InfoRow(
                        stringResource(R.string.hymofs_lkm_kmi_label),
                        config.unameRelease
                    )
                }
            }
        }

        // Loading / how LKM is loaded + autoload toggle + Load/Unload buttons
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation()
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            text = stringResource(R.string.hymofs_lkm_loading_card_title),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold
                        )
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            text = stringResource(R.string.hymofs_lkm_loading_desc),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                    Switch(
                        checked = config.lkmAutoload,
                        onCheckedChange = onLkmAutoloadChanged
                    )
                }
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = stringResource(R.string.hymofs_lkm_autoload_hint),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                // Load/Unload buttons (when LKM mode applies)
                if (!config.hymofsBuiltin) {
                    Spacer(modifier = Modifier.height(12.dp))
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        if (hymofsStatus != HymoFSStatus.AVAILABLE) {
                            Button(
                                onClick = {
                                    coroutineScope.launch {
                                        if (HymoFSManager.loadLkm()) {
                                            onRefresh()
                                            snackbarHostState.showSnackbar(
                                                context.getString(R.string.hymofs_lkm_load_success)
                                            )
                                        } else {
                                            snackbarHostState.showSnackbar(
                                                context.getString(R.string.hymofs_lkm_load_failed)
                                            )
                                        }
                                    }
                                }
                            ) {
                                Text(stringResource(R.string.hymofs_lkm_load))
                            }
                        }
                        if (hymofsStatus == HymoFSStatus.AVAILABLE) {
                            OutlinedButton(
                                onClick = {
                                    coroutineScope.launch {
                                        if (HymoFSManager.unloadLkm()) {
                                            onRefresh()
                                            snackbarHostState.showSnackbar(
                                                context.getString(R.string.hymofs_lkm_unload_success)
                                            )
                                        } else {
                                            snackbarHostState.showSnackbar(
                                                context.getString(R.string.hymofs_lkm_unload_failed)
                                            )
                                        }
                                    }
                                }
                            ) {
                                Text(stringResource(R.string.hymofs_lkm_unload))
                            }
                        }
                    }
                }
            }
        }

        // Current LKM Hooks (when HymoFS available)
        if (hymofsStatus == HymoFSStatus.AVAILABLE) {
            var hooksExpanded by remember { mutableStateOf(false) }
            var hooksText by remember { mutableStateOf<String?>(null) }
            var hooksLoading by remember { mutableStateOf(false) }
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
                elevation = getCardElevation()
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { hooksExpanded = !hooksExpanded },
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = stringResource(R.string.hymofs_lkm_hooks_title),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold
                        )
                        Icon(
                            imageVector = if (hooksExpanded) Icons.Filled.ExpandLess else Icons.Filled.ExpandMore,
                            contentDescription = null
                        )
                    }
                    if (hooksExpanded) {
                        Spacer(modifier = Modifier.height(8.dp))
                        LaunchedEffect(hooksExpanded) {
                            if (hooksExpanded && hooksText == null && !hooksLoading) {
                                hooksLoading = true
                                hooksText = HymoFSManager.getLkmHooks()
                                hooksLoading = false
                            }
                        }
                        if (hooksLoading) {
                            CircularProgressIndicator(modifier = Modifier.size(24.dp))
                        } else {
                            val text = hooksText ?: ""
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.Top
                            ) {
                                Text(
                                    text = text.ifEmpty { stringResource(R.string.hymofs_lkm_hooks_empty) },
                                    style = MaterialTheme.typography.bodySmall,
                                    fontFamily = FontFamily.Monospace,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    modifier = Modifier.weight(1f)
                                )
                                IconButton(
                                    onClick = {
                                        coroutineScope.launch {
                                            hooksLoading = true
                                            hooksText = HymoFSManager.getLkmHooks()
                                            hooksLoading = false
                                        }
                                    }
                                ) {
                                    Icon(Icons.Filled.Refresh, contentDescription = stringResource(R.string.hymofs_lkm_hooks_refresh))
                                }
                            }
                        }
                    }
                }
            }
        }

        // KMI Selection card (when hymofs available - disabled by default)
        if (hymofsStatus == HymoFSStatus.AVAILABLE) {
            var kmiOverrideEnabled by remember { mutableStateOf(config.lkmKmiOverride.isNotEmpty()) }
            var kmiDropdownExpanded by remember { mutableStateOf(false) }
            val supportedKmis by produceState(initialValue = emptyList<String>()) {
                value = getSupportedKmis()
            }
            LaunchedEffect(config.lkmKmiOverride) {
                kmiOverrideEnabled = config.lkmKmiOverride.isNotEmpty()
            }
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
                elevation = getCardElevation()
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                text = stringResource(R.string.hymofs_lkm_kmi_selection_title),
                                style = MaterialTheme.typography.titleMedium,
                                fontWeight = FontWeight.Bold
                            )
                            Spacer(modifier = Modifier.height(4.dp))
                            Text(
                                text = stringResource(R.string.hymofs_lkm_kmi_override_desc),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        Switch(
                            checked = kmiOverrideEnabled,
                            onCheckedChange = { enabled ->
                                kmiOverrideEnabled = enabled
                                if (!enabled) {
                                    coroutineScope.launch {
                                        if (HymoFSManager.clearLkmKmiOverride()) {
                                            onRefresh()
                                            snackbarHostState.showSnackbar(
                                                context.getString(R.string.hymofs_lkm_kmi_cleared)
                                            )
                                        }
                                    }
                                }
                            }
                        )
                    }
                    if (kmiOverrideEnabled) {
                        Spacer(modifier = Modifier.height(12.dp))
                        ExposedDropdownMenuBox(
                            expanded = kmiDropdownExpanded,
                            onExpandedChange = { kmiDropdownExpanded = it }
                        ) {
                            OutlinedTextField(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable),
                                readOnly = true,
                                label = { Text(stringResource(R.string.hymofs_lkm_kmi_selection_label)) },
                                value = config.lkmKmiOverride.ifEmpty { stringResource(R.string.hymofs_lkm_kmi_selection_placeholder) },
                                onValueChange = {},
                                trailingIcon = {
                                    Icon(
                                        if (kmiDropdownExpanded) Icons.Filled.ArrowDropUp else Icons.Filled.ArrowDropDown,
                                        contentDescription = null
                                    )
                                }
                            )
                            ExposedDropdownMenu(
                                expanded = kmiDropdownExpanded,
                                onDismissRequest = { kmiDropdownExpanded = false }
                            ) {
                                if (supportedKmis.isEmpty()) {
                                    DropdownMenuItem(
                                        text = { Text(stringResource(R.string.hymofs_lkm_kmi_selection_empty)) },
                                        onClick = { }
                                    )
                                } else {
                                    supportedKmis.forEach { kmi ->
                                        DropdownMenuItem(
                                            text = { Text(kmi) },
                                            onClick = {
                                                coroutineScope.launch {
                                                    if (HymoFSManager.setLkmKmiOverride(kmi)) {
                                                        onRefresh()
                                                        kmiDropdownExpanded = false
                                                        snackbarHostState.showSnackbar(
                                                            context.getString(R.string.hymofs_lkm_kmi_set_success)
                                                        )
                                                    } else {
                                                        snackbarHostState.showSnackbar(
                                                            context.getString(R.string.hymofs_lkm_kmi_failed)
                                                        )
                                                    }
                                                }
                                            }
                                        )
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// Modules tab removed - mount config is now integrated into main module list (ModuleScreen)

// ==================== Settings Tab ====================
@Composable
private fun SettingsTab(
    config: HymoFSManager.HymoConfig,
    hymofsStatus: HymoFSStatus,
    features: HymoFSManager.FeaturesResult?,
    snackbarHostState: SnackbarHostState,
    onConfigChanged: (HymoFSManager.HymoConfig) -> Unit,
    onSetDebug: (Boolean) -> Unit,
    onSetStealth: (Boolean) -> Unit,
    onFixMounts: () -> Unit
) {
    val coroutineScope = rememberCoroutineScope()
    val context = LocalContext.current
    var userHideRules by remember { mutableStateOf<List<String>>(emptyList()) }
    var newHideRule by remember { mutableStateOf("") }

    LaunchedEffect(Unit) {
        userHideRules = HymoFSManager.listUserHideRules()
    }
    
    // Helper to update config and save immediately
    fun updateAndSave(newConfig: HymoFSManager.HymoConfig) {
        onConfigChanged(newConfig)
    }
    
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        // General Settings
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation()
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text(
                    text = stringResource(R.string.hymofs_general),
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(bottom = 12.dp)
                )
                
                SettingSwitch(
                    title = stringResource(R.string.hymofs_debug),
                    subtitle = stringResource(R.string.hymofs_debug_desc),
                    checked = config.debug,
                    onCheckedChange = {
                        updateAndSave(config.copy(debug = it))
                    }
                )

                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

                SettingSwitch(
                    title = stringResource(R.string.hymofs_verbose),
                    subtitle = stringResource(R.string.hymofs_verbose_desc),
                    checked = config.verbose,
                    onCheckedChange = {
                        updateAndSave(config.copy(verbose = it))
                    }
                )
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                // Filesystem preference (fs_type) – auto / ext4 / erofs / tmpfs, like webui
                Text(
                    text = stringResource(R.string.hymofs_fs_type_title),
                    style = MaterialTheme.typography.bodyLarge
                )
                Spacer(modifier = Modifier.height(4.dp))
                Column(
                    modifier = Modifier.fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    listOf(
                        "auto" to R.string.hymofs_fs_type_auto,
                        "ext4" to R.string.hymofs_fs_type_ext4,
                        "erofs" to R.string.hymofs_fs_type_erofs,
                        "tmpfs" to R.string.hymofs_fs_type_tmpfs
                    ).forEach { (value, labelRes) ->
                        val selected = config.fsType == value
                        FilledTonalButton(
                            onClick = { updateAndSave(config.copy(fsType = value)) },
                            modifier = Modifier.fillMaxWidth(),
                            colors = if (selected) {
                                ButtonDefaults.filledTonalButtonColors(
                                    containerColor = MaterialTheme.colorScheme.primary,
                                    contentColor = MaterialTheme.colorScheme.onPrimary
                                )
                            } else {
                                ButtonDefaults.filledTonalButtonColors(
                                    containerColor = MaterialTheme.colorScheme.surfaceVariant,
                                    contentColor = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                        ) {
                            Text(stringResource(labelRes))
                        }
                    }
                }
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                SettingSwitch(
                    title = stringResource(R.string.hymofs_disable_umount),
                    subtitle = stringResource(R.string.hymofs_disable_umount_desc),
                    checked = config.disableUmount,
                    onCheckedChange = {
                        updateAndSave(config.copy(disableUmount = it))
                    }
                )

                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

                // Global user-defined hide rules (moved from Rules tab)
                Text(
                    text = stringResource(R.string.hymofs_user_hide_title),
                    style = MaterialTheme.typography.bodyLarge
                )
                Spacer(modifier = Modifier.height(4.dp))

                if (userHideRules.isEmpty()) {
                    Text(
                        text = stringResource(R.string.hymofs_user_hide_empty),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                } else {
                    Column(
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        userHideRules.forEach { path ->
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text(
                                    text = path,
                                    style = MaterialTheme.typography.bodySmall,
                                    fontFamily = FontFamily.Monospace,
                                    modifier = Modifier.weight(1f)
                                )
                                IconButton(onClick = {
                                    coroutineScope.launch {
                                        val ok = HymoFSManager.removeUserHideRule(path)
                                        if (ok) {
                                            userHideRules =
                                                HymoFSManager.listUserHideRules()
                                        } else {
                                            snackbarHostState.showSnackbar(
                                                context.getString(R.string.hymofs_user_hide_remove_failed)
                                            )
                                        }
                                    }
                                }) {
                                    Icon(
                                        imageVector = Icons.Filled.Delete,
                                        contentDescription = null
                                    )
                                }
                            }
                        }
                    }
                }

                Spacer(modifier = Modifier.height(8.dp))

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    OutlinedTextField(
                        value = newHideRule,
                        onValueChange = { newHideRule = it },
                        modifier = Modifier.weight(1f),
                        placeholder = {
                            Text(stringResource(R.string.hymofs_user_hide_placeholder))
                        },
                        singleLine = true
                    )
                    Button(
                        onClick = {
                            val path = newHideRule.trim()
                            if (path.isEmpty()) return@Button
                            coroutineScope.launch {
                                val ok = HymoFSManager.addUserHideRule(path)
                                if (ok) {
                                    userHideRules = HymoFSManager.listUserHideRules()
                                    newHideRule = ""
                                } else {
                                    snackbarHostState.showSnackbar(
                                        context.getString(R.string.hymofs_user_hide_add_failed)
                                    )
                                }
                            }
                        }
                    ) {
                        Text(stringResource(R.string.hymofs_user_hide_add))
                    }
                }
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                // Module Directory Setting
                var moduledir by remember { mutableStateOf(config.moduledir) }
                SettingTextField(
                    title = stringResource(R.string.hymofs_moduledir),
                    subtitle = stringResource(R.string.hymofs_moduledir_desc),
                    value = moduledir,
                    onValueChange = { moduledir = it },
                    onConfirm = {
                        updateAndSave(config.copy(moduledir = moduledir))
                    }
                )
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                // OverlayFS Mount Source Setting
                var mountsource by remember { mutableStateOf(config.mountsource) }
                SettingTextField(
                    title = stringResource(R.string.hymofs_mountsource),
                    subtitle = stringResource(R.string.hymofs_mountsource_desc),
                    value = mountsource,
                    onValueChange = { mountsource = it },
                    onConfirm = {
                        updateAndSave(config.copy(mountsource = mountsource))
                    },
                    placeholder = "KSU"
                )
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                // Partitions Setting with chip input
                PartitionsInput(
                    partitions = config.partitions,
                    onPartitionsChanged = { newPartitions ->
                        updateAndSave(config.copy(partitions = newPartitions))
                    },
                    onScanPartitions = {
                        coroutineScope.launch {
                            val newConfig = HymoFSManager.syncPartitionsWithDaemon()
                            if (newConfig != null) {
                                updateAndSave(newConfig)
                                snackbarHostState.showSnackbar(
                                    "Partitions synced from daemon"
                                )
                            } else {
                                snackbarHostState.showSnackbar("No new partitions found")
                            }
                        }
                    }
                )
            }
        }

        // Maps spoof card (when kernel supports maps_spoof)
        if (hymofsStatus == HymoFSStatus.AVAILABLE && features?.names?.contains("maps_spoof") == true) {
            MapsSpoofCard(
                snackbarHostState = snackbarHostState
            )
        }
        
        // Advanced Settings
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation()
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text(
                    text = stringResource(R.string.hymofs_advanced),
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(bottom = 12.dp)
                )
                
                val hymofsAvailable = hymofsStatus == HymoFSStatus.AVAILABLE
                
                // Global HymoFS enable (config flag, applied on daemon start)
                SettingSwitch(
                    title = stringResource(R.string.hymofs_enable_title),
                    subtitle = stringResource(R.string.hymofs_enable_desc),
                    checked = config.hymofsEnabled,
                    onCheckedChange = {
                        updateAndSave(config.copy(hymofsEnabled = it))
                    }
                )
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                SettingSwitch(
                    title = stringResource(R.string.hymofs_kernel_debug),
                    subtitle = stringResource(R.string.hymofs_kernel_debug_desc),
                    checked = config.enableKernelDebug,
                    enabled = hymofsAvailable,
                    onCheckedChange = {
                        onSetDebug(it)
                        updateAndSave(config.copy(enableKernelDebug = it))
                    }
                )
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                SettingSwitch(
                    title = stringResource(R.string.hymofs_stealth),
                    subtitle = stringResource(R.string.hymofs_stealth_desc),
                    checked = config.enableStealth,
                    enabled = hymofsAvailable,
                    onCheckedChange = {
                        onSetStealth(it)
                        updateAndSave(config.copy(enableStealth = it))
                    }
                )
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                SettingSwitch(
                    title = stringResource(R.string.hymofs_hidexattr),
                    subtitle = stringResource(R.string.hymofs_hidexattr_desc),
                    checked = config.enableHidexattr,
                    enabled = hymofsAvailable,
                    onCheckedChange = {
                        updateAndSave(config.copy(enableHidexattr = it))
                    }
                )
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                SettingSwitch(
                    title = stringResource(R.string.hymofs_enable_nuke),
                    subtitle = stringResource(R.string.hymofs_enable_nuke_desc),
                    checked = config.enableNuke,
                    enabled = hymofsAvailable,
                    onCheckedChange = {
                        updateAndSave(config.copy(enableNuke = it))
                    }
                )
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
                SettingSwitch(
                    title = stringResource(R.string.hymofs_ignore_protocol),
                    subtitle = stringResource(R.string.hymofs_ignore_protocol_desc),
                    checked = config.ignoreProtocolMismatch,
                    onCheckedChange = {
                        updateAndSave(config.copy(ignoreProtocolMismatch = it))
                    }
                )

                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

                // Mirror path / mount base: Auto (highest priority = user set; else auto = HymoFS? /dev/hymo_mirror : img_mnt)
                var mirrorPath by remember { mutableStateOf(config.mirrorPath) }
                val presetImgMnt = "/data/adb/hymo/img_mnt"
                val presetDebugRamdisk = "/debug_ramdisk"
                val presetDevMirror = "/dev/hymo_mirror"

                val selectedPreset = when {
                    mirrorPath.isEmpty() -> "auto"
                    mirrorPath == presetImgMnt -> "img_mnt"
                    mirrorPath == presetDebugRamdisk -> "debug_ramdisk"
                    mirrorPath == presetDevMirror -> "dev_mirror"
                    else -> "custom"
                }

                Text(
                    text = stringResource(R.string.hymofs_mirror_path),
                    style = MaterialTheme.typography.bodyLarge
                )
                Spacer(modifier = Modifier.height(4.dp))

                Column(
                    modifier = Modifier.fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    listOf(
                        "auto" to R.string.hymofs_mirror_preset_auto,
                        "img_mnt" to R.string.hymofs_mirror_preset_img_mnt,
                        "debug_ramdisk" to R.string.hymofs_mirror_preset_debug_ramdisk,
                        "dev_mirror" to R.string.hymofs_mirror_preset_dev_mirror,
                        "custom" to R.string.hymofs_mirror_preset_custom
                    ).forEach { (key, labelRes) ->
                        val selected = selectedPreset == key
                        FilledTonalButton(
                            onClick = {
                                when (key) {
                                    "auto" -> {
                                        mirrorPath = ""
                                        updateAndSave(config.copy(mirrorPath = ""))
                                    }
                                    "img_mnt" -> {
                                        mirrorPath = presetImgMnt
                                        updateAndSave(config.copy(mirrorPath = presetImgMnt))
                                    }
                                    "debug_ramdisk" -> {
                                        mirrorPath = presetDebugRamdisk
                                        updateAndSave(config.copy(mirrorPath = presetDebugRamdisk))
                                    }
                                    "dev_mirror" -> {
                                        mirrorPath = presetDevMirror
                                        updateAndSave(config.copy(mirrorPath = presetDevMirror))
                                    }
                                    "custom" -> {
                                        // Switch to custom mode; actual value edited below
                                        if (selectedPreset != "custom" && mirrorPath in listOf(
                                                presetImgMnt,
                                                presetDebugRamdisk,
                                                presetDevMirror
                                            )
                                        ) {
                                            mirrorPath = ""
                                        }
                                    }
                                }
                            },
                            modifier = Modifier.fillMaxWidth(),
                            enabled = (key == "auto" || key == "custom") || hymofsAvailable,
                            colors = if (selected) {
                                ButtonDefaults.filledTonalButtonColors(
                                    containerColor = MaterialTheme.colorScheme.primary,
                                    contentColor = MaterialTheme.colorScheme.onPrimary
                                )
                            } else {
                                ButtonDefaults.filledTonalButtonColors(
                                    containerColor = MaterialTheme.colorScheme.surfaceVariant,
                                    contentColor = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                        ) {
                            Text(stringResource(labelRes))
                        }
                    }
                }

                // Static description of presets (like hymo webui)
                Text(
                    text = stringResource(R.string.hymofs_mirror_path_desc),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )

                if (selectedPreset == "custom") {
                    SettingTextField(
                        title = "",
                        subtitle = "",
                        value = mirrorPath,
                        onValueChange = { mirrorPath = it },
                        onConfirm = {
                            updateAndSave(config.copy(mirrorPath = mirrorPath))
                        }
                    )
                }

                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

                // Temporary Mount Point Setting (moved down, below mirror presets/custom)
                var tempdir by remember { mutableStateOf(config.tempdir) }
                SettingTextField(
                    title = stringResource(R.string.hymofs_tempdir),
                    subtitle = stringResource(R.string.hymofs_tempdir_desc),
                    value = tempdir,
                    onValueChange = { tempdir = it },
                    onConfirm = {
                        updateAndSave(config.copy(tempdir = tempdir))
                    },
                    placeholder = "Auto"
                )

                if (hymofsAvailable) {
                    Spacer(modifier = Modifier.height(12.dp))

                    OutlinedButton(
                        onClick = onFixMounts,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Icon(Icons.Filled.Build, contentDescription = null)
                        Spacer(modifier = Modifier.width(8.dp))
                        Text(stringResource(R.string.hymofs_fix_mounts))
                    }
                }
            }
        }
    }
}

@Composable
private fun SettingSwitch(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    enabled: Boolean = true
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyLarge,
                color = if (enabled) Color.Unspecified else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
            )
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) 1f else 0.5f)
            )
        }
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            enabled = enabled
        )
    }
}

@Composable
private fun SettingTextField(
    title: String,
    subtitle: String,
    value: String,
    onValueChange: (String) -> Unit,
    onConfirm: () -> Unit,
    enabled: Boolean = true,
    placeholder: String = ""
) {
    var isEditing by remember { mutableStateOf(false) }
    
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.bodyLarge,
                    color = if (enabled) Color.Unspecified else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
                )
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) 1f else 0.5f)
                )
            }
            IconButton(
                onClick = { isEditing = !isEditing },
                enabled = enabled
            ) {
                Icon(
                    imageVector = if (isEditing) Icons.Filled.Check else Icons.Filled.Edit,
                    contentDescription = null
                )
            }
        }
        
        if (isEditing) {
            OutlinedTextField(
                value = value,
                onValueChange = onValueChange,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 8.dp),
                placeholder = { if (placeholder.isNotEmpty()) Text(placeholder) },
                singleLine = true,
                enabled = enabled,
                trailingIcon = {
                    IconButton(onClick = {
                        onConfirm()
                        isEditing = false
                    }) {
                        Icon(Icons.Filled.Done, contentDescription = null)
                    }
                }
            )
        } else if (value.isNotEmpty()) {
            Text(
                text = value,
                modifier = Modifier.padding(top = 4.dp),
                style = MaterialTheme.typography.bodyMedium,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.primary
            )
        }
    }
}

// ==================== Maps spoof card (Settings) ====================
@Composable
private fun MapsSpoofCard(
    snackbarHostState: SnackbarHostState
) {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    var showClearConfirm by remember { mutableStateOf(false) }
    var showAddDialog by remember { mutableStateOf(false) }

    if (showClearConfirm) {
        AlertDialog(
            onDismissRequest = { showClearConfirm = false },
            title = { Text(stringResource(R.string.hymofs_maps_clear)) },
            text = { Text(stringResource(R.string.hymofs_maps_clear_confirm)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        showClearConfirm = false
                        coroutineScope.launch {
                            if (HymoFSManager.clearMapsRules()) {
                                snackbarHostState.showSnackbar(context.getString(R.string.hymofs_maps_cleared))
                            } else {
                                snackbarHostState.showSnackbar(context.getString(R.string.hymofs_maps_clear_failed))
                            }
                        }
                    }
                ) {
                    Text(stringResource(R.string.hymofs_rules_clear), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearConfirm = false }) {
                    Text(stringResource(R.string.hymofs_rules_cancel))
                }
            }
        )
    }

    if (showAddDialog) {
        var tIno by remember { mutableStateOf("") }
        var tDev by remember { mutableStateOf("0") }
        var sIno by remember { mutableStateOf("") }
        var sDev by remember { mutableStateOf("0") }
        var path by remember { mutableStateOf("") }
        AlertDialog(
            onDismissRequest = { showAddDialog = false },
            title = { Text(stringResource(R.string.hymofs_maps_add_rule)) },
            text = {
                Column(
                    modifier = Modifier.fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    OutlinedTextField(
                        value = tIno,
                        onValueChange = { tIno = it },
                        label = { Text(stringResource(R.string.hymofs_maps_target_ino)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                    OutlinedTextField(
                        value = tDev,
                        onValueChange = { tDev = it },
                        label = { Text(stringResource(R.string.hymofs_maps_target_dev)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                    OutlinedTextField(
                        value = sIno,
                        onValueChange = { sIno = it },
                        label = { Text(stringResource(R.string.hymofs_maps_spoof_ino)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                    OutlinedTextField(
                        value = sDev,
                        onValueChange = { sDev = it },
                        label = { Text(stringResource(R.string.hymofs_maps_spoof_dev)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                    OutlinedTextField(
                        value = path,
                        onValueChange = { path = it },
                        label = { Text(stringResource(R.string.hymofs_maps_spoof_path)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        val ti = tIno.trim().toLongOrNull()
                        val td = tDev.trim().toLongOrNull() ?: 0L
                        val si = sIno.trim().toLongOrNull()
                        val sd = sDev.trim().toLongOrNull() ?: 0L
                        val p = path.trim()
                        if (ti != null && si != null && p.isNotEmpty()) {
                            showAddDialog = false
                            coroutineScope.launch {
                                if (HymoFSManager.addMapsRule(ti, td, si, sd, p)) {
                                    snackbarHostState.showSnackbar(context.getString(R.string.hymofs_maps_add_success))
                                } else {
                                    snackbarHostState.showSnackbar(context.getString(R.string.hymofs_maps_add_failed))
                                }
                            }
                        }
                    }
                ) {
                    Text(stringResource(R.string.hymofs_maps_add_rule))
                }
            },
            dismissButton = {
                TextButton(onClick = { showAddDialog = false }) {
                    Text(stringResource(R.string.hymofs_rules_cancel))
                }
            }
        )
    }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = getCardElevation()
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = stringResource(R.string.hymofs_maps_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(bottom = 4.dp)
            )
            Text(
                text = stringResource(R.string.hymofs_maps_desc),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(bottom = 12.dp)
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                OutlinedButton(
                    onClick = { showClearConfirm = true },
                    colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                    modifier = Modifier.weight(1f)
                ) {
                    Icon(Icons.Filled.Delete, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(stringResource(R.string.hymofs_maps_clear))
                }
                FilledTonalButton(
                    onClick = { showAddDialog = true },
                    modifier = Modifier.weight(1f)
                ) {
                    Icon(Icons.Filled.Add, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(stringResource(R.string.hymofs_maps_add_rule))
                }
            }
        }
    }
}

// ==================== Rules Tab ====================
@Composable
private fun RulesTab(
    activeRules: List<HymoFSManager.ActiveRule>,
    hymofsStatus: HymoFSStatus,
    onRefresh: () -> Unit,
    onClearAll: () -> Unit
) {
    var showClearDialog by remember { mutableStateOf(false) }

    if (showClearDialog) {
        AlertDialog(
            onDismissRequest = { showClearDialog = false },
            title = { Text(stringResource(R.string.hymofs_rules_clear_all)) },
            text = { Text(stringResource(R.string.hymofs_rules_clear_confirm)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        showClearDialog = false
                        onClearAll()
                    }
                ) {
                    Text(stringResource(R.string.hymofs_rules_clear), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearDialog = false }) {
                    Text(stringResource(R.string.hymofs_rules_cancel))
                }
            }
        )
    }
    
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        if (hymofsStatus != HymoFSStatus.AVAILABLE) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
                elevation = getCardElevation()
            ) {
                Row(
                    modifier = Modifier.padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(Icons.Filled.Info, contentDescription = null)
                    Text(stringResource(R.string.hymofs_rules_not_available))
                }
            }
            return
        }

        // Action buttons
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            OutlinedButton(
                onClick = onRefresh,
                modifier = Modifier.weight(1f)
            ) {
                Icon(Icons.Filled.Refresh, contentDescription = null)
                Spacer(modifier = Modifier.width(8.dp))
                Text(stringResource(R.string.hymofs_rules_refresh))
            }
            
            OutlinedButton(
                onClick = { showClearDialog = true },
                modifier = Modifier.weight(1f),
                colors = ButtonDefaults.outlinedButtonColors(
                    contentColor = MaterialTheme.colorScheme.error
                )
            ) {
                Icon(Icons.Filled.Delete, contentDescription = null)
                Spacer(modifier = Modifier.width(8.dp))
                Text(stringResource(R.string.hymofs_rules_clear_all))
            }
        }
        
        // Rules list
        Text(
            text = stringResource(R.string.hymofs_rules_count, activeRules.size),
            style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(bottom = 8.dp)
        )
        
        LazyColumn(
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(activeRules) { rule ->
                RuleItem(rule)
            }
            
            if (activeRules.isEmpty()) {
                item {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(32.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = stringResource(R.string.hymofs_rules_empty),
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun RuleItem(rule: HymoFSManager.ActiveRule) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(8.dp),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerHigh),
        elevation = getCardElevation()
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Surface(
                shape = RoundedCornerShape(4.dp),
                color = when (rule.type) {
                    "add" -> Color(0xFF1B5E20).copy(alpha = 0.3f)
                    "hide" -> Color(0xFFB71C1C).copy(alpha = 0.3f)
                    "inject" -> Color(0xFF1565C0).copy(alpha = 0.3f)
                    "merge" -> Color(0xFF4A148C).copy(alpha = 0.3f)
                    "mount_hide" -> Color(0xFF0D47A1).copy(alpha = 0.3f)
                    "maps_spoof" -> Color(0xFF1A237E).copy(alpha = 0.3f)
                    "statfs_spoof" -> Color(0xFF311B92).copy(alpha = 0.3f)
                    "stealth" -> Color(0xFF37474F).copy(alpha = 0.3f)
                    else -> MaterialTheme.colorScheme.secondaryContainer
                }
            ) {
                Text(
                    text = when (rule.type) {
                        "mount_hide" -> "MOUNT_HIDE"
                        "maps_spoof" -> "MAPS_SPOOF"
                        "statfs_spoof" -> "STATFS_SPOOF"
                        "stealth" -> "STEALTH"
                        else -> rule.type.uppercase()
                    },
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold
                )
            }
            
            Column(modifier = Modifier.weight(1f)) {
                val displayText = when (rule.type) {
                    "mount_hide" -> stringResource(R.string.hymofs_rule_mount_hide)
                    "maps_spoof" -> stringResource(R.string.hymofs_rule_maps_spoof)
                    "statfs_spoof" -> stringResource(R.string.hymofs_rule_statfs_spoof)
                    "stealth" -> stringResource(R.string.hymofs_rule_stealth)
                    else -> rule.src
                }
                Text(
                    text = displayText,
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = if (rule.type in listOf("mount_hide", "maps_spoof", "statfs_spoof", "stealth"))
                        FontFamily.Default else FontFamily.Monospace,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
                if (rule.target != null) {
                    Text(
                        text = "→ ${rule.target}",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }
    }
}

// ==================== Logs Tab ====================

// Log level colors aligned with webui: V=purple, D=green, I=blue, W=orange, E=red, other=white
enum class LogLevel(val displayNameRes: Int, val color: Color, val tag: String) {
    VERBOSE(R.string.hymofs_logs_filter_verbose, Color(0xFF9C27B0), "VERBOSE"),
    DEBUG(R.string.hymofs_logs_filter_debug, Color(0xFF4CAF50), "DEBUG"),
    INFO(R.string.hymofs_logs_filter_info, Color(0xFF2196F3), "INFO"),
    WARN(R.string.hymofs_logs_filter_warn, Color(0xFFFF9800), "WARN"),
    ERROR(R.string.hymofs_logs_filter_error, Color(0xFFF44336), "ERROR")
}

@Composable
private fun LogsTab(
    showKernelLog: Boolean,
    onToggleLogType: () -> Unit,
    logContent: String,
    onRefreshLog: () -> Unit
) {
    val context = LocalContext.current
    val snackbarHostState = remember { SnackbarHostState() }
    val coroutineScope = rememberCoroutineScope()
    // Default: show all. When non-empty, show only selected levels (unselected = hidden, e.g. no verbose if not selected)
    var selectedLogLevels by remember { mutableStateOf(emptySet<LogLevel>()) }
    var filterExpanded by remember { mutableStateOf(false) }
    
    LaunchedEffect(showKernelLog) {
        onRefreshLog()
    }
    
    // Filter logs: empty set = all; non-empty = only lines matching any selected level
    val filteredLogContent = remember(logContent, selectedLogLevels) {
        if (selectedLogLevels.isEmpty()) {
            logContent
        } else {
            logContent.lines()
                .filter { line ->
                    selectedLogLevels.any { level -> line.contains(level.tag, ignoreCase = true) }
                }
                .joinToString("\n")
        }
    }
    
    // Parse and colorize logs (V紫 D绿 I蓝 W橙 E红 其余白, same as webui)
    val annotatedLogContent = remember(filteredLogContent) {
        buildAnnotatedString {
            filteredLogContent.lines().forEach { line ->
                val color = when {
                    line.contains("ERROR", ignoreCase = true) -> LogLevel.ERROR.color   // E red
                    line.contains("WARN", ignoreCase = true) -> LogLevel.WARN.color    // W orange
                    line.contains("INFO", ignoreCase = true) -> LogLevel.INFO.color    // I blue
                    line.contains("DEBUG", ignoreCase = true) -> LogLevel.DEBUG.color  // D green
                    line.contains("VERBOSE", ignoreCase = true) -> LogLevel.VERBOSE.color // V purple
                    else -> Color(0xFFFFFFFF)
                }
                withStyle(style = SpanStyle(color = color)) {
                    append(line)
                }
                append("\n")
            }
        }
    }
    
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        // Control buttons row
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Log type toggles
            FilterChip(
                selected = !showKernelLog,
                onClick = { if (showKernelLog) onToggleLogType() },
                label = { Text(stringResource(R.string.hymofs_logs_daemon)) }
            )
            FilterChip(
                selected = showKernelLog,
                onClick = { if (!showKernelLog) onToggleLogType() },
                label = { Text(stringResource(R.string.hymofs_logs_kernel)) }
            )
            
            Spacer(modifier = Modifier.weight(1f))
            
            // Copy button
            IconButton(
                onClick = {
                    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
                    val clip = android.content.ClipData.newPlainText("HymoFS Log", filteredLogContent)
                    clipboard.setPrimaryClip(clip)
                    coroutineScope.launch {
                        snackbarHostState.showSnackbar(context.getString(R.string.hymofs_logs_copy_success))
                    }
                }
            ) {
                Icon(Icons.Filled.ContentCopy, contentDescription = stringResource(R.string.hymofs_logs_copy))
            }
            
            // Filter icon (popup multi-select): default all, unselected levels hidden
            Box {
                IconButton(onClick = { filterExpanded = true }) {
                    Icon(
                        Icons.Filled.FilterList,
                        contentDescription = stringResource(R.string.hymofs_logs_filter)
                    )
                }
                DropdownMenu(
                    expanded = filterExpanded,
                    onDismissRequest = { filterExpanded = false },
                    modifier = Modifier.widthIn(min = 180.dp)
                ) {
                    Column(
                        modifier = Modifier.padding(vertical = 8.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp)
                    ) {
                        FilterChip(
                            selected = selectedLogLevels.isEmpty(),
                            onClick = {
                                selectedLogLevels = emptySet()
                                filterExpanded = false
                            },
                            label = { Text(stringResource(R.string.hymofs_logs_filter_all)) },
                            modifier = Modifier.padding(horizontal = 12.dp)
                        )
                        HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))
                        FlowRow(
                            modifier = Modifier.padding(horizontal = 12.dp),
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalArrangement = Arrangement.spacedBy(6.dp)
                        ) {
                            LogLevel.entries.forEach { level ->
                                FilterChip(
                                    selected = level in selectedLogLevels,
                                    onClick = {
                                        selectedLogLevels = if (level in selectedLogLevels) {
                                            selectedLogLevels - level
                                        } else {
                                            selectedLogLevels + level
                                        }
                                    },
                                    label = { Text(stringResource(level.displayNameRes)) },
                                    leadingIcon = {
                                        Box(
                                            modifier = Modifier
                                                .size(8.dp)
                                                .background(level.color, shape = RoundedCornerShape(4.dp))
                                        )
                                    }
                                )
                            }
                        }
                    }
                }
            }
            IconButton(onClick = onRefreshLog) {
                Icon(Icons.Filled.Refresh, contentDescription = "Refresh")
            }
        }
        
        // Log content
        Card(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
            shape = RoundedCornerShape(16.dp),
            colors = CardDefaults.cardColors(
                containerColor = Color(0xFF1E1E1E)
            )
        ) {
            Box(modifier = Modifier.fillMaxSize()) {
                val scrollState = rememberScrollState()
                
                Text(
                    text = annotatedLogContent,
                    modifier = Modifier
                        .fillMaxSize()
                        .verticalScroll(scrollState)
                        .padding(12.dp),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp
                )
                
                SnackbarHost(
                    hostState = snackbarHostState,
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .padding(16.dp)
                )
            }
        }
    }
}

// ==================== Partitions Input Component ====================
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PartitionsInput(
    partitions: List<String>,
    onPartitionsChanged: (List<String>) -> Unit,
    onScanPartitions: () -> Unit
) {
    var inputText by remember { mutableStateOf("") }
    
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.hymofs_partitions),
                    style = MaterialTheme.typography.bodyLarge
                )
                Text(
                    text = stringResource(R.string.hymofs_partitions_desc),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            IconButton(onClick = onScanPartitions) {
                Icon(Icons.Filled.Refresh, contentDescription = stringResource(R.string.hymofs_partitions_scan))
            }
        }
        
        // Chips display with FlowRow for wrapping
        if (partitions.isNotEmpty()) {
            FlowRow(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                partitions.forEach { partition ->
                    InputChip(
                        selected = false,
                        onClick = { },
                        label = { Text(partition) },
                        trailingIcon = {
                            Icon(
                                imageVector = Icons.Filled.Close,
                                contentDescription = "Remove",
                                modifier = Modifier
                                    .size(18.dp)
                                    .clickable {
                                        onPartitionsChanged(partitions - partition)
                                    }
                            )
                        }
                    )
                }
            }
        }
        
        // Input field for adding new partitions
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            OutlinedTextField(
                value = inputText,
                onValueChange = { inputText = it },
                modifier = Modifier.weight(1f),
                placeholder = { Text(stringResource(R.string.hymofs_partitions_placeholder)) },
                singleLine = true,
                trailingIcon = {
                    if (inputText.isNotEmpty()) {
                        IconButton(
                            onClick = {
                                val newPartitions = inputText
                                    .split(",", " ")
                                    .map { it.trim() }
                                    .filter { it.isNotEmpty() }
                                
                                if (newPartitions.isNotEmpty()) {
                                    val merged = (partitions + newPartitions).distinct()
                                    onPartitionsChanged(merged)
                                    inputText = ""
                                }
                            }
                        ) {
                            Icon(Icons.Filled.Add, contentDescription = stringResource(R.string.hymofs_partitions_add))
                        }
                    }
                }
            )
        }
    }
}
