package com.anatdx.yukisu.ui.hymofs

import android.annotation.SuppressLint
import android.content.Context
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
    MODULES(R.string.hymofs_tab_modules),
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
    var logContent by remember { mutableStateOf("") }
    var showKernelLog by remember { mutableStateOf(false) }
    var builtinMountEnabled by remember { mutableStateOf(true) }

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
                builtinMountEnabled = HymoFSManager.isBuiltinMountEnabled()
                if (hymofsStatus == HymoFSStatus.AVAILABLE) {
                    activeRules = HymoFSManager.getActiveRules()
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
                        version = version,
                        systemInfo = systemInfo,
                        storageInfo = storageInfo,
                        modules = modules,
                        onRefresh = { loadData() }
                    )
                    HymoFSTab.MODULES -> ModulesTab(
                        modules = modules,
                        hymofsAvailable = hymofsStatus == HymoFSStatus.AVAILABLE,
                        onModeChanged = { moduleId, mode ->
                            coroutineScope.launch {
                                if (HymoFSManager.setModuleMode(moduleId, mode)) {
                                    snackbarHostState.showSnackbar(
                                        context.getString(R.string.hymofs_toast_mode_updated)
                                    )
                                    loadData()
                                } else {
                                    snackbarHostState.showSnackbar(
                                        context.getString(R.string.hymofs_toast_mode_failed)
                                    )
                                }
                            }
                        }
                    )
                    HymoFSTab.SETTINGS -> SettingsTab(
                        config = config,
                        hymofsStatus = hymofsStatus,
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
                        },
                        builtinMountEnabled = builtinMountEnabled,
                        onBuiltinMountChanged = { enable ->
                            coroutineScope.launch {
                                if (HymoFSManager.setBuiltinMountEnabled(enable)) {
                                    builtinMountEnabled = enable
                                    val msgRes = if (enable) {
                                        R.string.hymofs_toast_builtin_enabled
                                    } else {
                                        R.string.hymofs_toast_builtin_disabled
                                    }
                                    snackbarHostState.showSnackbar(context.getString(msgRes))
                                } else {
                                    snackbarHostState.showSnackbar(
                                        context.getString(R.string.hymofs_toast_builtin_failed)
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
    version: String,
    systemInfo: HymoFSManager.SystemInfo,
    storageInfo: HymoFSManager.StorageInfo,
    modules: List<HymoFSManager.ModuleInfo>,
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
                                when (hymofsStatus) {
                                    HymoFSStatus.AVAILABLE -> R.string.hymofs_status_available
                                    HymoFSStatus.NOT_PRESENT -> R.string.hymofs_status_not_present
                                    HymoFSStatus.KERNEL_TOO_OLD -> R.string.hymofs_status_kernel_too_old
                                    HymoFSStatus.MODULE_TOO_OLD -> R.string.hymofs_status_module_too_old
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

// ==================== Modules Tab ====================
@Composable
private fun ModulesTab(
    modules: List<HymoFSManager.ModuleInfo>,
    hymofsAvailable: Boolean,
    onModeChanged: (String, String) -> Unit
) {
    val modes = if (hymofsAvailable) {
        listOf("auto", "hymofs", "overlay", "magic", "none")
    } else {
        listOf("auto", "overlay", "magic", "none")
    }
    
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        items(modules) { module ->
            ModuleCard(
                module = module,
                modes = modes,
                onModeChanged = { mode -> onModeChanged(module.id, mode) }
            )
        }
        
        if (modules.isEmpty()) {
            item {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(32.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "No modules found",
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ModuleCard(
    module: HymoFSManager.ModuleInfo,
    modes: List<String>,
    onModeChanged: (String) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    var rulesExpanded by remember { mutableStateOf(false) }
    var localRules by remember { mutableStateOf(module.rules) }
    var newRulePath by remember { mutableStateOf("") }
    var newRuleMode by remember { mutableStateOf("auto") }
    val coroutineScope = rememberCoroutineScope()
    val context = LocalContext.current
    
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
                        text = module.name,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        text = module.id,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        fontFamily = FontFamily.Monospace
                    )
                }
                
                // Strategy badge
                Surface(
                    shape = RoundedCornerShape(4.dp),
                    color = when (module.strategy) {
                        "hymofs" -> Color(0xFF1B5E20).copy(alpha = 0.3f)
                        "overlay" -> MaterialTheme.colorScheme.primaryContainer
                        "magic" -> Color(0xFF4A148C).copy(alpha = 0.3f)
                        else -> MaterialTheme.colorScheme.surfaceVariant
                    }
                ) {
                    Text(
                        text = module.strategy.uppercase(),
                        modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.Bold
                    )
                }
            }
            
            if (module.version.isNotEmpty() || module.author.isNotEmpty()) {
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = buildString {
                        if (module.version.isNotEmpty()) append("v${module.version}")
                        if (module.author.isNotEmpty()) {
                            if (isNotEmpty()) append(" • ")
                            append(module.author)
                        }
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            Spacer(modifier = Modifier.height(12.dp))
            
            // Mode selector
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    text = stringResource(id = R.string.hymofs_module_mode),
                    style = MaterialTheme.typography.bodyMedium
                )
                
                ExposedDropdownMenuBox(
                    expanded = expanded,
                    onExpandedChange = { expanded = !expanded },
                    modifier = Modifier.weight(1f)
                ) {
                    OutlinedTextField(
                            value = modeLabel(module.mode),
                        onValueChange = {},
                        readOnly = true,
                        trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) },
                        modifier = Modifier.menuAnchor(),
                        singleLine = true,
                        textStyle = MaterialTheme.typography.bodyMedium
                    )
                    
                    ExposedDropdownMenu(
                        expanded = expanded,
                        onDismissRequest = { expanded = false }
                    ) {
                        modes.forEach { mode ->
                            DropdownMenuItem(
                                text = { Text(modeLabel(mode)) },
                                onClick = {
                                    expanded = false
                                    if (mode != module.mode) {
                                        onModeChanged(mode)
                                    }
                                }
                            )
                        }
                    }
                }
            }

            // Custom per-module rules (like hymo module UI)
            if (localRules.isNotEmpty() || true) {
                Spacer(modifier = Modifier.height(12.dp))
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { rulesExpanded = !rulesExpanded },
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = stringResource(id = R.string.hymofs_module_rules_title),
                        style = MaterialTheme.typography.bodyMedium,
                        fontWeight = FontWeight.Medium
                    )
                    Icon(
                        imageVector = if (rulesExpanded) Icons.Filled.ExpandLess else Icons.Filled.ExpandMore,
                        contentDescription = null
                    )
                }

                if (rulesExpanded) {
                    Spacer(modifier = Modifier.height(8.dp))

                    if (localRules.isEmpty()) {
                        Text(
                            text = stringResource(id = R.string.hymofs_module_rules_empty),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    } else {
                        Column(
                            verticalArrangement = Arrangement.spacedBy(4.dp),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            localRules.forEach { rule ->
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    verticalAlignment = Alignment.CenterVertically,
                                    horizontalArrangement = Arrangement.SpaceBetween
                                ) {
                                    Column(
                                        modifier = Modifier.weight(1f)
                                    ) {
                                        Text(
                                            text = rule.path,
                                            style = MaterialTheme.typography.bodySmall,
                                            fontFamily = FontFamily.Monospace
                                        )
                                        Text(
                                            text = modeLabel(rule.mode),
                                            style = MaterialTheme.typography.labelSmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    }
                                    IconButton(onClick = {
                                        coroutineScope.launch {
                                            val ok = HymoFSManager.removeModuleRule(
                                                module.id,
                                                rule.path
                                            )
                                            if (ok) {
                                                localRules =
                                                    localRules.filterNot { it.path == rule.path }
                                            } else {
                                                Toast
                                                    .makeText(
                                                        context,
                                                        context.getString(R.string.hymofs_module_rules_remove_failed),
                                                        Toast.LENGTH_SHORT
                                                    )
                                                    .show()
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

                    // 输入一行路径，下一行是「规则 + 按钮」
                    Column(
                        modifier = Modifier.fillMaxWidth(),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        OutlinedTextField(
                            value = newRulePath,
                            onValueChange = { newRulePath = it },
                            modifier = Modifier.fillMaxWidth(),
                            placeholder = {
                                Text(
                                    text = stringResource(id = R.string.hymofs_module_rules_placeholder)
                                )
                            },
                            singleLine = true
                        )

                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            var ruleModeExpanded by remember { mutableStateOf(false) }

                            ExposedDropdownMenuBox(
                                expanded = ruleModeExpanded,
                                onExpandedChange = { ruleModeExpanded = !ruleModeExpanded },
                                modifier = Modifier.weight(1f)
                            ) {
                                OutlinedTextField(
                                    value = modeLabel(newRuleMode),
                                    onValueChange = {},
                                    readOnly = true,
                                    trailingIcon = {
                                        ExposedDropdownMenuDefaults.TrailingIcon(
                                            expanded = ruleModeExpanded
                                        )
                                    },
                                    modifier = Modifier
                                        .menuAnchor()
                                        .fillMaxWidth(),
                                    singleLine = true,
                                    textStyle = MaterialTheme.typography.bodyMedium
                                )

                                ExposedDropdownMenu(
                                    expanded = ruleModeExpanded,
                                    onDismissRequest = { ruleModeExpanded = false }
                                ) {
                                    modes.forEach { m ->
                                        DropdownMenuItem(
                                            text = { Text(modeLabel(m)) },
                                            onClick = {
                                                newRuleMode = m
                                                ruleModeExpanded = false
                                            }
                                        )
                                    }
                                }
                            }

                            Button(
                                onClick = {
                                    val path = newRulePath.trim()
                                    if (path.isEmpty()) return@Button
                                    coroutineScope.launch {
                                        val ok =
                                            HymoFSManager.addModuleRule(
                                                module.id,
                                                path,
                                                newRuleMode
                                            )
                                        if (ok) {
                                            localRules =
                                                localRules +
                                                    HymoFSManager.ModuleRule(
                                                        path,
                                                        newRuleMode
                                                    )
                                            newRulePath = ""
                                        } else {
                                            Toast
                                                .makeText(
                                                    context,
                                                    context.getString(
                                                        R.string.hymofs_module_rules_add_failed
                                                    ),
                                                    Toast.LENGTH_SHORT
                                                )
                                                .show()
                                        }
                                    }
                                }
                            ) {
                                Text(text = stringResource(id = R.string.hymofs_module_rules_add))
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun modeLabel(mode: String): String {
    return when (mode) {
        "auto" -> stringResource(id = R.string.hymofs_mode_auto)
        "hymofs" -> stringResource(id = R.string.hymofs_mode_hymofs)
        "overlay" -> stringResource(id = R.string.hymofs_mode_overlay)
        "magic" -> stringResource(id = R.string.hymofs_mode_magic)
        "none" -> stringResource(id = R.string.hymofs_mode_none)
        else -> mode
    }
}

// ==================== Settings Tab ====================
@Composable
private fun SettingsTab(
    config: HymoFSManager.HymoConfig,
    hymofsStatus: HymoFSStatus,
    snackbarHostState: SnackbarHostState,
    onConfigChanged: (HymoFSManager.HymoConfig) -> Unit,
    onSetDebug: (Boolean) -> Unit,
    onSetStealth: (Boolean) -> Unit,
    onFixMounts: () -> Unit,
    builtinMountEnabled: Boolean,
    onBuiltinMountChanged: (Boolean) -> Unit
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
                    title = stringResource(R.string.hymofs_builtin_mount),
                    subtitle = stringResource(R.string.hymofs_builtin_mount_desc),
                    checked = builtinMountEnabled,
                    onCheckedChange = onBuiltinMountChanged
                )
                
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
                
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

                // Kernel uname spoofing (release + version)
                Text(
                    text = stringResource(R.string.hymofs_uname_title),
                    style = MaterialTheme.typography.bodyLarge
                )
                Spacer(modifier = Modifier.height(4.dp))

                var unameRelease by remember { mutableStateOf(config.unameRelease) }
                SettingTextField(
                    title = stringResource(R.string.hymofs_uname_release),
                    subtitle = stringResource(R.string.hymofs_uname_release_desc),
                    value = unameRelease,
                    onValueChange = { unameRelease = it },
                    onConfirm = {
                        updateAndSave(config.copy(unameRelease = unameRelease))
                    }
                )

                var unameVersion by remember { mutableStateOf(config.unameVersion) }
                SettingTextField(
                    title = stringResource(R.string.hymofs_uname_version),
                    subtitle = stringResource(R.string.hymofs_uname_version_desc),
                    value = unameVersion,
                    onValueChange = { unameVersion = it },
                    onConfirm = {
                        updateAndSave(config.copy(unameVersion = unameVersion))
                    }
                )

                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

                // Mirror path / mount base presets (like meta-hymo webui)
                var mirrorPath by remember { mutableStateOf(config.mirrorPath) }
                val effectiveMirrorPath = if (mirrorPath.isEmpty()) "/dev/hymo_mirror" else mirrorPath

                Text(
                    text = stringResource(R.string.hymofs_mirror_path),
                    style = MaterialTheme.typography.bodyLarge
                )
                Spacer(modifier = Modifier.height(4.dp))

                // Presets: /data/adb/hymo/img_mnt, /debug_ramdisk, /dev/hymo_mirror, plus Custom
                val presetImgMnt = "/data/adb/hymo/img_mnt"
                val presetDebugRamdisk = "/debug_ramdisk"
                val presetDevMirror = "/dev/hymo_mirror"

                val selectedPreset = when (effectiveMirrorPath) {
                    presetImgMnt -> "img_mnt"
                    presetDebugRamdisk -> "debug_ramdisk"
                    presetDevMirror -> "dev_mirror"
                    else -> "custom"
                }

                Column(
                    modifier = Modifier.fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    listOf(
                        "img_mnt" to R.string.hymofs_mirror_preset_img_mnt,
                        "debug_ramdisk" to R.string.hymofs_mirror_preset_debug_ramdisk,
                        "dev_mirror" to R.string.hymofs_mirror_preset_dev_mirror,
                        "custom" to R.string.hymofs_mirror_preset_custom
                    ).forEach { (key, labelRes) ->
                        val selected = selectedPreset == key
                        FilledTonalButton(
                            onClick = {
                                when (key) {
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
                            enabled = !(key != "custom" && !hymofsAvailable),
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

                // Static description of presets (like meta-hymo webui)
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

                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

                // Simple mount stage selector via long buttons
                Text(
                    text = stringResource(R.string.hymofs_mount_stage),
                    style = MaterialTheme.typography.bodyLarge
                )
                Spacer(modifier = Modifier.height(4.dp))
                Column(
                    modifier = Modifier.fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    listOf(
                        "post-fs-data" to R.string.hymofs_mount_stage_post_fs,
                        "metamount" to R.string.hymofs_mount_stage_meta,
                        "services" to R.string.hymofs_mount_stage_services
                    ).forEach { (value, labelRes) ->
                        val selected = config.mountStage == value
                        FilledTonalButton(
                            onClick = { updateAndSave(config.copy(mountStage = value)) },
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

                    Spacer(modifier = Modifier.height(8.dp))

                    OutlinedButton(
                        onClick = {
                            coroutineScope.launch {
                                val ok = HymoFSManager.createModulesImage()
                                snackbarHostState.showSnackbar(
                                    if (ok) "modules.img created" else "Failed to create modules.img"
                                )
                            }
                        },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Icon(Icons.Filled.Save, contentDescription = null)
                        Spacer(modifier = Modifier.width(8.dp))
                        Text(stringResource(R.string.hymofs_create_image))
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
                    else -> MaterialTheme.colorScheme.secondaryContainer
                }
            ) {
                Text(
                    text = rule.type.uppercase(),
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold
                )
            }
            
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = rule.src,
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
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

enum class LogLevel(val displayNameRes: Int, val color: Color, val tag: String) {
    ALL(R.string.hymofs_logs_filter_all, Color.Unspecified, ""),
    INFO(R.string.hymofs_logs_filter_info, Color(0xFF4CAF50), "INFO"),
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
    var selectedLogLevel by remember { mutableStateOf(LogLevel.ALL) }
    
    LaunchedEffect(showKernelLog) {
        onRefreshLog()
    }
    
    // Filter logs by level
    val filteredLogContent = remember(logContent, selectedLogLevel) {
        if (selectedLogLevel == LogLevel.ALL) {
            logContent
        } else {
            logContent.lines()
                .filter { line -> line.contains(selectedLogLevel.tag, ignoreCase = true) }
                .joinToString("\n")
        }
    }
    
    // Parse and colorize logs
    val annotatedLogContent = remember(filteredLogContent) {
        buildAnnotatedString {
            filteredLogContent.lines().forEach { line ->
                val color = when {
                    line.contains("ERROR", ignoreCase = true) -> LogLevel.ERROR.color
                    line.contains("WARN", ignoreCase = true) -> LogLevel.WARN.color
                    line.contains("INFO", ignoreCase = true) -> LogLevel.INFO.color
                    else -> Color(0xFFD4D4D4)
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
            
            // Refresh button
            IconButton(onClick = onRefreshLog) {
                Icon(Icons.Filled.Refresh, contentDescription = "Refresh")
            }
        }
        
        // Log level filter chips
        LazyRow(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(LogLevel.entries.size) { index ->
                val level = LogLevel.entries[index]
                FilterChip(
                    selected = selectedLogLevel == level,
                    onClick = { selectedLogLevel = level },
                    label = { Text(stringResource(level.displayNameRes)) },
                    leadingIcon = if (level != LogLevel.ALL) {
                        {
                            Box(
                                modifier = Modifier
                                    .size(8.dp)
                                    .background(level.color, shape = RoundedCornerShape(4.dp))
                            )
                        }
                    } else null
                )
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
