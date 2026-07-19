package com.anatdx.yukisu.ui.screen

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.NavigateNext
import androidx.compose.material.icons.automirrored.filled.Undo
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.rounded.FolderDelete
import androidx.compose.material.icons.rounded.RemoveCircle
import androidx.compose.material.icons.rounded.RemoveModerator
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import androidx.core.content.edit
import com.maxkeppeker.sheets.core.models.base.IconSource
import com.maxkeppeler.sheets.list.models.ListOption
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.AppProfileTemplateScreenDestination
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.LogViewerScreenDestination
import com.ramcosta.composedestinations.generated.destinations.UmountManagerScreenDestination
import com.ramcosta.composedestinations.generated.destinations.MoreSettingsScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.magica.MagicaHelper
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.*
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.CardConfig.cardAlpha
import com.anatdx.yukisu.ui.theme.ExpressiveListGroupMinHeight
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

/**
 * @author ShirkNeko
 * @date 2025/9/29.
 */
private val SPACING_SMALL = 3.dp
private val SPACING_MEDIUM = 8.dp
private val SPACING_LARGE = 16.dp

enum class SettingsItemPosition(val index: Int, val count: Int) {
    First(0, 3),
    Middle(1, 3),
    Last(2, 3),
    Only(0, 1)
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun SettingScreen(navigator: DestinationsNavigator) {
    val topAppBarState = rememberTopAppBarState()
    val scrollBehavior = if (isExpressiveUi) {
        TopAppBarDefaults.exitUntilCollapsedScrollBehavior(topAppBarState)
    } else {
        TopAppBarDefaults.pinnedScrollBehavior(topAppBarState)
    }
    val snackBarHost = remember { SnackbarHostState() }
    val context = LocalContext.current
    val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
    val isKsuManager = remember { Natives.isManager }
    var isSuLogEnabled by remember { mutableStateOf(Natives.isSuLogEnabled()) }
    var selectedEngine by rememberSaveable {
        mutableStateOf(
            prefs.getString("webui_engine", "default") ?: "default"
        )
    }
    var autoJailbreak by rememberSaveable {
        mutableStateOf(MagicaHelper.isAutoJailbreakEnabled(context))
    }

    Scaffold(
        topBar = {
            TopBar(scrollBehavior = scrollBehavior)
        },
        snackbarHost = { SnackbarHost(snackBarHost) },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
    ) { paddingValues ->
        val aboutDialog = rememberCustomDialog {
            AboutDialog(it)
        }
        val loadingDialog = rememberLoadingDialog()

        Column(
            modifier = Modifier
                .padding(paddingValues)
                .nestedScroll(scrollBehavior.nestedScrollConnection)
                .verticalScroll(rememberScrollState())
        ) {
            val context = LocalContext.current
            val scope = rememberCoroutineScope()
            val exportBugreportLauncher = rememberLauncherForActivityResult(
                ActivityResultContracts.CreateDocument("application/gzip")
            ) { uri: Uri? ->
                if (uri == null) return@rememberLauncherForActivityResult
                scope.launch(Dispatchers.IO) {
                    loadingDialog.show()
                    context.contentResolver.openOutputStream(uri)?.use { output ->
                        getBugreportFile(context).inputStream().use {
                            it.copyTo(output)
                        }
                    }
                    loadingDialog.hide()
                    snackBarHost.showSnackbar(context.getString(R.string.log_saved))
                }
            }

            KsuIsValid {
                SettingsGroupCard(
                    title = stringResource(R.string.configuration),
                    content = {
                        SettingItem(
                            icon = Icons.Filled.Fence,
                            title = stringResource(R.string.settings_profile_template),
                            summary = stringResource(R.string.settings_profile_template_summary),
                            groupPosition = SettingsItemPosition.First,
                            onClick = {
                                navigator.navigate(AppProfileTemplateScreenDestination)
                            }
                        )

                        // 不保存 SuperKey
                        val superKeyPrefs = context.getSharedPreferences("superkey", Context.MODE_PRIVATE)
                        var skipStoreSuperKey by remember {
                            mutableStateOf(superKeyPrefs.getBoolean("skip_store_superkey", false))
                        }
                        SwitchItem(
                            icon = Icons.Filled.Key,
                            title = stringResource(R.string.settings_donot_store_superkey),
                            summary = stringResource(R.string.settings_donot_store_superkey_summary),
                            checked = skipStoreSuperKey,
                            onCheckedChange = {
                                skipStoreSuperKey = it
                                superKeyPrefs.edit().putBoolean("skip_store_superkey", it).apply()
                                if (it) {
                                    // 如果开启了不保存，立即清除已保存的密钥
                                    superKeyPrefs.edit().remove("saved_superkey").apply()
                                }
                            }
                        )

                        // 清除 SuperKey
                        val clearKeyDialog = rememberConfirmDialog(onConfirm = {
                            superKeyPrefs.edit().remove("saved_superkey").apply()
                            scope.launch {
                                snackBarHost.showSnackbar(context.getString(R.string.clear_super_key) + " ✓")
                            }
                        })
                        val clearKeyDialogTitle = stringResource(R.string.clear_super_key)
                        val clearKeyDialogContent = stringResource(R.string.settings_clear_super_key_dialog)
                        SettingItem(
                            icon = Icons.Filled.Key,
                            title = stringResource(R.string.clear_super_key),
                            onClick = {
                                clearKeyDialog.showConfirm(
                                    title = clearKeyDialogTitle,
                                    content = clearKeyDialogContent,
                                    markdown = false
                                )
                            }
                        )

                        var selinuxHideEnabled by remember {
                            mutableStateOf(Natives.isSelinuxHideEnabled())
                        }
                        val selinuxHideStatus by produceState(initialValue = "") {
                            value = getFeatureStatus("selinux_hide")
                        }
                        val selinuxHideSummary = when (selinuxHideStatus) {
                            "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                            "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                            else -> stringResource(id = R.string.settings_selinux_hide_summary)
                        }
                        SwitchItem(
                            icon = Icons.Filled.Security,
                            title = stringResource(id = R.string.settings_selinux_hide),
                            summary = selinuxHideSummary,
                            checked = selinuxHideEnabled,
                            enabled = selinuxHideStatus == "supported",
                            onCheckedChange = { enabled ->
                                val ok = Natives.setSelinuxHideEnabled(enabled)
                                if (ok) {
                                    execKsud("feature save", true)
                                    selinuxHideEnabled = enabled
                                }
                                scope.launch {
                                    snackBarHost.showSnackbar(
                                        context.getString(
                                            if (ok) R.string.setting_change_saved_reboot
                                            else R.string.setting_change_failed
                                        )
                                    )
                                }
                            }
                        )

                        var suCompatDisabled by remember {
                            mutableStateOf(!Natives.isSuEnabled())
                        }
                        val suStatus by produceState(initialValue = "") {
                            value = getFeatureStatus("su_compat")
                        }
                        val suSummary = when (suStatus) {
                            "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                            "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                            else -> stringResource(id = R.string.settings_disable_su_summary)
                        }
                        SwitchItem(
                            icon = Icons.Rounded.RemoveModerator,
                            title = stringResource(id = R.string.settings_disable_su),
                            summary = suSummary,
                            checked = suCompatDisabled,
                            enabled = suStatus == "supported",
                            onCheckedChange = { disabled ->
                                if (Natives.setSuEnabled(!disabled)) {
                                    execKsud("feature save", true)
                                    suCompatDisabled = !Natives.isSuEnabled()
                                }
                            }
                        )

                        var kernelUmountDisabled by remember {
                            mutableStateOf(!Natives.isKernelUmountEnabled())
                        }
                        val umountStatus by produceState(initialValue = "") {
                            value = getFeatureStatus("kernel_umount")
                        }
                        val umountSummary = when (umountStatus) {
                            "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                            "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                            else -> stringResource(id = R.string.settings_disable_kernel_umount_summary)
                        }
                        SwitchItem(
                            icon = Icons.Rounded.RemoveCircle,
                            title = stringResource(id = R.string.settings_disable_kernel_umount),
                            summary = umountSummary,
                            checked = kernelUmountDisabled,
                            enabled = umountStatus == "supported",
                            onCheckedChange = { disabled ->
                                if (Natives.setKernelUmountEnabled(!disabled)) {
                                    execKsud("feature save", true)
                                    kernelUmountDisabled = !Natives.isKernelUmountEnabled()
                                }
                            }
                        )

                        var suLogEnabled by remember {
                            mutableStateOf(Natives.isSuLogEnabled())
                        }
                        val suLogStatus by produceState(initialValue = "") {
                            value = getFeatureStatus("sulog")
                        }
                        val suLogSummary = when (suLogStatus) {
                            "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                            "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                            else -> stringResource(id = R.string.settings_disable_sulog_summary)
                        }
                        SwitchItem(
                            icon = Icons.Filled.Visibility,
                            title = stringResource(id = R.string.settings_disable_sulog),
                            summary = suLogSummary,
                            checked = suLogEnabled,
                            enabled = suLogStatus == "supported",
                            onCheckedChange = { enabled ->
                                if (Natives.setSuLogEnabled(enabled)) {
                                    execKsud("feature save", true)
                                    suLogEnabled = Natives.isSuLogEnabled()
                                    isSuLogEnabled = suLogEnabled
                                }
                            }
                        )

                        var adbRootEnabled by rememberSaveable {
                            mutableStateOf(Natives.isAdbRootEnabled())
                        }
                        val adbRootStatus by produceState(initialValue = "") {
                            value = getFeatureStatus("adb_root")
                        }
                        val adbRootSummary = when (adbRootStatus) {
                            "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                            "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                            else -> stringResource(id = R.string.settings_adb_root_summary)
                        }
                        SwitchItem(
                            icon = Icons.Filled.DeveloperMode,
                            title = stringResource(id = R.string.settings_adb_root),
                            summary = adbRootSummary,
                            checked = adbRootEnabled,
                            enabled = adbRootStatus == "supported",
                            onCheckedChange = { enabled ->
                                if (Natives.setAdbRootEnabled(enabled)) {
                                    execKsud("feature save", true)
                                    restartAdbd()
                                    adbRootEnabled = enabled
                                }
                            }
                        )

                        // 卸载模块开关
                        var umountChecked by rememberSaveable { mutableStateOf(Natives.isDefaultUmountModules()) }
                        SwitchItem(
                            icon = Icons.Rounded.FolderDelete,
                            title = stringResource(id = R.string.settings_umount_modules_default),
                            summary = stringResource(id = R.string.settings_umount_modules_default_summary),
                            checked = umountChecked,
                            onCheckedChange = {
                                if (Natives.setDefaultUmountModules(it)) {
                                    umountChecked = it
                                }
                            }
                        )

                        // app profile 防逃逸：全局默认 NO_NEW_PRIVS（含默认档/manager/shell）
                        var defaultNnpChecked by rememberSaveable {
                            mutableStateOf(Natives.isDefaultNoNewPrivsEnabled())
                        }
                        SwitchItem(
                            icon = Icons.Filled.FrontHand,
                            title = stringResource(id = R.string.settings_default_no_new_privs),
                            summary = stringResource(id = R.string.settings_default_no_new_privs_summary),
                            checked = defaultNnpChecked,
                            onCheckedChange = {
                                if (Natives.setDefaultNoNewPrivsEnabled(it)) {
                                    execKsud("feature save", true)
                                    defaultNnpChecked = it
                                }
                            }
                        )

                        // YukiZygisk：内核捕获 zygote、注入 zygisk 模块（接全局防逃逸之后）
                        var yukiZygiskEnabled by remember { mutableStateOf(false) }
                        val yukiZygiskStatus by produceState(initialValue = "") {
                            value = getFeatureStatus("yukizygisk")
                        }
                        LaunchedEffect(Unit) {
                            yukiZygiskEnabled = getFeatureValue("yukizygisk")
                        }
                        val yukiZygiskSummary = when (yukiZygiskStatus) {
                            "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                            "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                            else -> stringResource(id = R.string.settings_yukizygisk_summary)
                        }
                        SwitchItem(
                            icon = Icons.Filled.Extension,
                            title = stringResource(id = R.string.settings_yukizygisk),
                            summary = yukiZygiskSummary,
                            checked = yukiZygiskEnabled,
                            enabled = yukiZygiskStatus == "supported",
                            groupPosition = SettingsItemPosition.Last,
                            onCheckedChange = { enable ->
                                // toggle UX：先翻到用户意图，再异步落地 + toast，失败回滚
                                yukiZygiskEnabled = enable
                                scope.launch {
                                    if (setFeatureValue("yukizygisk", enable)) {
                                        snackBarHost.showSnackbar(
                                            context.getString(
                                                if (enable) R.string.settings_yukizygisk_toast_on
                                                else R.string.settings_yukizygisk_toast_off
                                            )
                                        )
                                    } else {
                                        yukiZygiskEnabled = getFeatureValue("yukizygisk")
                                        snackBarHost.showSnackbar(
                                            context.getString(R.string.settings_yukizygisk_toast_failed)
                                        )
                                    }
                                }
                            }
                        )

                    }
                )
            }

            SettingsGroupCard(
                title = stringResource(R.string.app_settings),
                content = {
                    var checkUpdate by rememberSaveable {
                        mutableStateOf(prefs.getBoolean("check_update", true))
                    }
                    SwitchItem(
                        icon = Icons.Filled.Update,
                        title = stringResource(R.string.settings_check_update),
                        summary = stringResource(R.string.settings_check_update_summary),
                        checked = checkUpdate,
                        groupPosition = SettingsItemPosition.First,
                        onCheckedChange = { enabled ->
                            prefs.edit { putBoolean("check_update", enabled) }
                            checkUpdate = enabled
                        }
                    )

                    var autoUpdateKsud by rememberSaveable {
                        mutableStateOf(prefs.getBoolean("auto_update_ksud", false))
                    }
                    SwitchItem(
                        icon = Icons.Filled.Sync,
                        title = stringResource(R.string.settings_auto_update_ksud),
                        summary = stringResource(R.string.settings_auto_update_ksud_summary),
                        checked = autoUpdateKsud,
                        onCheckedChange = { enabled ->
                            prefs.edit { putBoolean("auto_update_ksud", enabled) }
                            autoUpdateKsud = enabled
                        }
                    )

                    // WebUI引擎选择
                    KsuIsValid {
                        WebUIEngineSelector(
                            selectedEngine = selectedEngine,
                            onEngineSelected = { engine ->
                                selectedEngine = engine
                                prefs.edit { putString("webui_engine", engine) }
                            }
                        )
                    }

                    SwitchItem(
                        icon = Icons.Filled.ElectricalServices,
                        title = stringResource(R.string.settings_auto_jailbreak),
                        summary = stringResource(R.string.settings_auto_jailbreak_summary),
                        checked = autoJailbreak,
                        onCheckedChange = { enabled ->
                            MagicaHelper.setAutoJailbreakEnabled(context, enabled)
                            autoJailbreak = enabled
                        }
                    )

                    // 更多设置
                    SettingItem(
                        icon = Icons.Filled.Settings,
                        title = stringResource(R.string.more_settings),
                        summary = stringResource(R.string.more_settings),
                        groupPosition = SettingsItemPosition.Last,
                        onClick = {
                            navigator.navigate(MoreSettingsScreenDestination)
                        }
                    )
                }
            )

            // 工具卡片
            SettingsGroupCard(
                title = stringResource(R.string.tools),
                content = {
                    var showBottomsheet by remember { mutableStateOf(false) }

                    SettingItem(
                        icon = Icons.Filled.BugReport,
                        title = stringResource(R.string.send_log),
                        groupPosition = if (isKsuManager) {
                            SettingsItemPosition.First
                        } else {
                            SettingsItemPosition.Only
                        },
                        onClick = {
                            showBottomsheet = true
                        }
                    )

                    // 查看使用日志
                    KsuIsValid {
                        if (isSuLogEnabled) {
                            SettingItem(
                                icon = Icons.Filled.Visibility,
                                title = stringResource(R.string.log_viewer_view_logs),
                                summary = stringResource(R.string.log_viewer_view_logs_summary),
                                onClick = {
                                    navigator.navigate(LogViewerScreenDestination)
                                }
                            )
                        }
                    }
                    KsuIsValid {
                        SettingItem(
                            icon = Icons.Filled.FolderOff,
                            title = stringResource(R.string.umount_path_manager),
                            summary = stringResource(R.string.umount_path_manager_summary),
                            onClick = {
                                navigator.navigate(UmountManagerScreenDestination)
                            }
                        )
                    }

                    if (showBottomsheet) {
                        LogBottomSheet(
                            onDismiss = { showBottomsheet = false },
                            onSaveLog = {
                                val formatter = DateTimeFormatter.ofPattern("yyyy-MM-dd_HH_mm")
                                val current = LocalDateTime.now().format(formatter)
                                exportBugreportLauncher.launch("KernelSU_bugreport_${current}.tar.gz")
                                showBottomsheet = false
                            },
                            onShareLog = {
                                scope.launch {
                                    val bugreport = loadingDialog.withLoading {
                                        withContext(Dispatchers.IO) {
                                            getBugreportFile(context)
                                        }
                                    }

                                    val uri = FileProvider.getUriForFile(
                                        context,
                                        "${BuildConfig.APPLICATION_ID}.fileprovider",
                                        bugreport
                                    )

                                    val shareIntent = Intent(Intent.ACTION_SEND).apply {
                                        putExtra(Intent.EXTRA_STREAM, uri)
                                        setDataAndType(uri, "application/gzip")
                                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                    }

                                    context.startActivity(
                                        Intent.createChooser(
                                            shareIntent,
                                            context.getString(R.string.send_log)
                                        )
                                    )

                                    showBottomsheet = false
                                }
                            }
                        )
                    }
                    KsuIsValid {
                        UninstallItem(
                            navigator = navigator,
                            groupPosition = SettingsItemPosition.Last
                        ) {
                            loadingDialog.withLoading(it)
                        }
                    }
                }
            )

            // 关于卡片
            SettingsGroupCard(
                title = stringResource(R.string.about),
                content = {
                    SettingItem(
                        icon = Icons.Filled.Info,
                        title = stringResource(R.string.about),
                        groupPosition = SettingsItemPosition.Only,
                        onClick = {
                            aboutDialog.show()
                        }
                    )
                }
            )

            Spacer(modifier = Modifier.height(SPACING_LARGE))
        }
    }
}

@Composable
private fun SettingsGroupCard(
    title: String,
    content: @Composable ColumnScope.() -> Unit
) {
    if (isExpressiveUi) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = SPACING_LARGE, vertical = 12.dp)
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Normal,
                modifier = Modifier.padding(horizontal = 8.dp, vertical = SPACING_MEDIUM),
                color = MaterialTheme.colorScheme.primary
            )
            Column(content = content)
        }
    } else {
        Card(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = SPACING_LARGE, vertical = SPACING_MEDIUM),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation()
        ) {
            Column(modifier = Modifier.padding(vertical = SPACING_MEDIUM)) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(
                        horizontal = SPACING_LARGE,
                        vertical = SPACING_MEDIUM
                    ),
                    color = MaterialTheme.colorScheme.primary
                )
                content()
            }
        }
    }
}

@Composable
private fun WebUIEngineSelector(
    selectedEngine: String,
    onEngineSelected: (String) -> Unit
) {
    var showDialog by remember { mutableStateOf(false) }
    val engineOptions = listOf(
        "default" to stringResource(R.string.engine_auto_select),
        "wx" to stringResource(R.string.engine_force_webuix),
        "ksu" to stringResource(R.string.engine_force_ksu)
    )

    SettingItem(
        icon = Icons.Filled.WebAsset,
        title = stringResource(R.string.use_webuix),
        summary = engineOptions.find { it.first == selectedEngine }?.second
            ?: stringResource(R.string.engine_auto_select),
        onClick = { showDialog = true }
    )

    if (showDialog) {
        YukiAlertDialog(
            onDismissRequest = { showDialog = false },
            title = { Text(stringResource(R.string.use_webuix)) },
            text = {
                Column {
                    engineOptions.forEach { (value, label) ->
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable {
                                    onEngineSelected(value)
                                    showDialog = false
                                }
                                .padding(vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(
                                selected = selectedEngine == value,
                                onClick = null
                            )
                            Spacer(modifier = Modifier.width(SPACING_MEDIUM))
                            Text(text = label)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showDialog = false }) {
                    Text(stringResource(R.string.cancel))
                }
            }
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LogBottomSheet(
    onDismiss: () -> Unit,
    onSaveLog: () -> Unit,
    onShareLog: () -> Unit
) {
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        modifier = Modifier.clickHapticFeedback(),
        containerColor = MaterialTheme.colorScheme.surfaceContainerHigh,
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(SPACING_LARGE),
            horizontalArrangement = Arrangement.SpaceEvenly
        ) {
            LogActionButton(
                icon = Icons.Filled.Save,
                text = stringResource(R.string.save_log),
                onClick = onSaveLog
            )

            LogActionButton(
                icon = Icons.Filled.Share,
                text = stringResource(R.string.send_log),
                onClick = onShareLog
            )
        }
        Spacer(modifier = Modifier.height(SPACING_LARGE))
    }
}

@Composable
fun LogActionButton(
    icon: ImageVector,
    text: String,
    onClick: () -> Unit
) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier
            .clickable(onClick = onClick)
            .padding(SPACING_MEDIUM)
    ) {
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .size(56.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.primaryContainer)
        ) {
            YukiIcon(
                imageVector = icon,
                contentDescription = text,
                tint = MaterialTheme.colorScheme.onPrimaryContainer,
                modifier = Modifier.size(24.dp)
            )
        }
        Spacer(modifier = Modifier.height(SPACING_MEDIUM))
        Text(
            text = text,
            style = MaterialTheme.typography.bodyMedium
        )
    }
}

@Composable
fun SettingItem(
    icon: ImageVector,
    title: String,
    summary: String? = null,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    onClick: () -> Unit
) {
    val expressiveShape = if (groupPosition == SettingsItemPosition.Only) {
        MaterialTheme.shapes.large
    } else {
        ListItemDefaults.segmentedShapes(groupPosition.index, groupPosition.count).shape
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .then(
                if (isExpressiveUi) {
                    Modifier
                        .padding(
                            horizontal = 6.dp,
                            vertical = ListItemDefaults.SegmentedGap / 2
                        )
                        .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                        .clip(expressiveShape)
                        .background(
                            MaterialTheme.colorScheme.surfaceContainer.copy(alpha = cardAlpha)
                        )
                        .clickable(onClick = onClick)
                } else {
                    Modifier.clickable(onClick = onClick)
                }
            )
            .padding(horizontal = SPACING_LARGE, vertical = 12.dp),
        verticalAlignment = if (isExpressiveUi) Alignment.CenterVertically else Alignment.Top
    ) {
        SettingsLeadingIcon(icon = icon, tint = MaterialTheme.colorScheme.primary)

        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = if (isExpressiveUi) FontWeight.Normal else null
            )
            if (summary != null) {
                Spacer(modifier = Modifier.height(SPACING_SMALL))
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (isExpressiveUi) {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    } else {
                        Color.Unspecified
                    }
                )
            }
        }
        YukiIcon(
            imageVector = Icons.AutoMirrored.Filled.NavigateNext,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(24.dp)
        )
    }
}

@Composable
fun SwitchItem(
    icon: ImageVector,
    title: String,
    summary: String? = null,
    checked: Boolean,
    enabled: Boolean = true,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    onCheckedChange: (Boolean) -> Unit
) {
    val expressiveShape = if (groupPosition == SettingsItemPosition.Only) {
        MaterialTheme.shapes.large
    } else {
        ListItemDefaults.segmentedShapes(groupPosition.index, groupPosition.count).shape
    }
    val titleColor = if (enabled) {
        MaterialTheme.colorScheme.onSurface
    } else {
        MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
    }
    val summaryColor = if (enabled) {
        MaterialTheme.colorScheme.onSurfaceVariant
    } else {
        MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
    }
    val iconTint = when {
        !enabled -> MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
        checked -> MaterialTheme.colorScheme.primary
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .then(
                if (isExpressiveUi) {
                    Modifier
                        .padding(
                            horizontal = 6.dp,
                            vertical = ListItemDefaults.SegmentedGap / 2
                        )
                        .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                        .clip(expressiveShape)
                        .background(
                            MaterialTheme.colorScheme.surfaceContainer.copy(alpha = cardAlpha)
                        )
                        .clickable(enabled = enabled) { onCheckedChange(!checked) }
                } else {
                    Modifier.clickable(enabled = enabled) { onCheckedChange(!checked) }
                }
            )
            .padding(horizontal = SPACING_LARGE, vertical = 12.dp),
        verticalAlignment = if (isExpressiveUi) Alignment.CenterVertically else Alignment.Top
    ) {
        SettingsLeadingIcon(icon = icon, tint = iconTint)

        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
                color = titleColor
            )
            if (summary != null) {
                Spacer(modifier = Modifier.height(SPACING_SMALL))
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodyMedium,
                    color = summaryColor
                )
            }
        }
        YukiSwitch(
            checked = checked,
            enabled = enabled,
            onCheckedChange = onCheckedChange
        )
    }
}

@Composable
private fun SettingsLeadingIcon(icon: ImageVector, tint: Color) {
    if (isExpressiveUi) {
        YukiIcon(
            imageVector = icon,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier
                .padding(end = SPACING_LARGE)
                .size(24.dp)
        )
    } else {
        YukiIcon(
            imageVector = icon,
            contentDescription = null,
            tint = tint,
            modifier = Modifier
                .padding(end = SPACING_LARGE)
                .size(24.dp)
        )
    }
}

@Composable
fun UninstallItem(
    navigator: DestinationsNavigator,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    withLoading: suspend (suspend () -> Unit) -> Unit
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val uninstallConfirmDialog = rememberConfirmDialog()
    val uninstallDialog = rememberUninstallDialog { uninstallType ->
        scope.launch {
            val result = uninstallConfirmDialog.awaitConfirm(
                title = context.getString(uninstallType.title),
                content = context.getString(uninstallType.message)
            )
            if (result == ConfirmResult.Confirmed) {
                withLoading {
                    when (uninstallType) {
                        UninstallType.PERMANENT -> navigator.navigate(
                            FlashScreenDestination(FlashIt.FlashUninstall)
                        )
                        UninstallType.RESTORE_STOCK_IMAGE -> navigator.navigate(
                            FlashScreenDestination(FlashIt.FlashRestore)
                        )
                        UninstallType.NONE -> Unit
                    }
                }
            }
        }
    }

    SettingItem(
        icon = Icons.Filled.Delete,
        title = stringResource(id = R.string.settings_uninstall),
        groupPosition = groupPosition,
        onClick = {
            uninstallDialog.show()
        }
    )
}

enum class UninstallType(val title: Int, val message: Int, val icon: ImageVector) {
    PERMANENT(
        R.string.settings_uninstall_permanent,
        R.string.settings_uninstall_permanent_message,
        Icons.Filled.DeleteForever
    ),
    RESTORE_STOCK_IMAGE(
        R.string.settings_restore_stock_image,
        R.string.settings_restore_stock_image_message,
        Icons.AutoMirrored.Filled.Undo
    ),
    NONE(0, 0, Icons.Filled.Delete)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun rememberUninstallDialog(onSelected: (UninstallType) -> Unit): DialogHandle {
    return rememberCustomDialog { dismiss ->
        val options = listOf(
            UninstallType.PERMANENT,
            UninstallType.RESTORE_STOCK_IMAGE
        )
        val listOptions = options.map {
            ListOption(
                titleText = stringResource(it.title),
                subtitleText = if (it.message != 0) stringResource(it.message) else null,
                icon = IconSource(it.icon)
            )
        }

        var selectedOption by remember { mutableStateOf<UninstallType?>(null) }

        MaterialTheme(
            colorScheme = MaterialTheme.colorScheme.copy(
                surface = MaterialTheme.colorScheme.surfaceContainerHigh
            )
        ) {
            YukiAlertDialog(
                onDismissRequest = {
                    dismiss()
                },
                title = {
                    Text(
                        text = stringResource(R.string.settings_uninstall),
                    )
                },
                text = {
                    Column(
                        modifier = Modifier.padding(vertical = 8.dp),
                        verticalArrangement = Arrangement.spacedBy(16.dp)
                    ) {
                        options.forEachIndexed { index, option ->
                            val isSelected = selectedOption == option
                            val backgroundColor = if (isSelected)
                                MaterialTheme.colorScheme.primaryContainer
                            else
                                Color.Transparent
                            val contentColor = if (isSelected)
                                MaterialTheme.colorScheme.onPrimaryContainer
                            else
                                MaterialTheme.colorScheme.onSurface

                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clip(MaterialTheme.shapes.medium)
                                    .background(backgroundColor)
                                    .clickable {
                                        selectedOption = option
                                    }
                                    .padding(vertical = 12.dp, horizontal = 8.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                YukiIcon(
                                    imageVector = option.icon,
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier
                                        .padding(end = 16.dp)
                                        .size(24.dp)
                                )
                                Column(
                                    modifier = Modifier.weight(1f)
                                ) {
                                    Text(
                                        text = listOptions[index].titleText,
                                        style = MaterialTheme.typography.titleMedium,
                                    )
                                    listOptions[index].subtitleText?.let {
                                        Text(
                                            text = it,
                                            style = MaterialTheme.typography.bodyMedium,
                                            color = if (isSelected)
                                                contentColor.copy(alpha = 0.8f)
                                            else
                                                MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    }
                                }
                                if (isSelected) {
                                    YukiIcon(
                                        imageVector = Icons.Default.RadioButtonChecked,
                                        contentDescription = null,
                                        tint = MaterialTheme.colorScheme.primary,
                                        modifier = Modifier.size(24.dp)
                                    )
                                } else {
                                    YukiIcon(
                                        imageVector = Icons.Default.RadioButtonUnchecked,
                                        contentDescription = null,
                                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                        modifier = Modifier.size(24.dp)
                                    )
                                }
                            }
                        }
                    }
                },
                confirmButton = {
                    Button(
                        onClick = {
                            selectedOption?.let { onSelected(it) }
                            dismiss()
                        },
                        enabled = selectedOption != null,
                    ) {
                        Text(
                            text = stringResource(android.R.string.ok)
                        )
                    }
                },
                dismissButton = {
                    TextButton(
                        onClick = {
                            dismiss()
                        }
                    ) {
                        Text(
                            text = stringResource(android.R.string.cancel),
                        )
                    }
                },
                shape = MaterialTheme.shapes.extraLarge,
                tonalElevation = 4.dp
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    scrollBehavior: TopAppBarScrollBehavior? = null
) {
    val colorScheme = MaterialTheme.colorScheme
    val cardColor = if (CardConfig.isCustomBackgroundEnabled) {
        colorScheme.surfaceContainerLow
    } else {
        colorScheme.background
    }
    val colors = TopAppBarDefaults.topAppBarColors(
        containerColor = cardColor,
        scrolledContainerColor = cardColor
    )
    val title: @Composable () -> Unit = {
        Text(
            text = stringResource(R.string.settings),
            fontWeight = if (isExpressiveUi) FontWeight.Normal else null
        )
    }

    if (isExpressiveUi) {
        LargeFlexibleTopAppBar(
            title = title,
            colors = colors,
            windowInsets = WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal
            ),
            scrollBehavior = scrollBehavior
        )
    } else {
        TopAppBar(
            title = title,
            colors = colors,
            windowInsets = WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal
            ),
            scrollBehavior = scrollBehavior
        )
    }
}
