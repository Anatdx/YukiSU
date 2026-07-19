package com.anatdx.yukisu.ui.screen

import android.annotation.SuppressLint
import android.content.Context
import android.os.Build
import android.os.PowerManager
import android.system.Os
import android.widget.Toast
import androidx.annotation.StringRes
import androidx.compose.animation.*
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CornerBasedShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Engineering
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.outlined.Block
import androidx.compose.material.icons.outlined.TaskAlt
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.content.pm.PackageInfoCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.InstallScreenDestination
import com.ramcosta.composedestinations.generated.destinations.YukiZygiskScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.KernelVersion
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.magica.MagicaHelper
import com.anatdx.yukisu.ui.component.KsuIsValid
import com.anatdx.yukisu.ui.component.rememberConfirmDialog
import com.anatdx.yukisu.ui.component.rememberLoadingDialog
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.component.YukiPullToRefreshBox
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.clickHapticFeedback
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.CardConfig.cardElevation
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.checkNewVersion
import com.anatdx.yukisu.ui.util.module.LatestVersionInfo
import com.anatdx.yukisu.ui.util.reboot
import com.anatdx.yukisu.ui.viewmodel.HomeViewModel
import com.anatdx.yukisu.ui.component.SuperKeyDialog
import com.anatdx.yukisu.ui.component.rememberSuperKeyDialog
import com.anatdx.yukisu.ui.component.SuperKeyAuthResult
import com.anatdx.yukisu.ui.util.KsuCli
import com.anatdx.yukisu.ui.activity.util.AppData
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.random.Random

/**
 * @author ShirkNeko
 * @date 2025/9/29.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>(start = true)
@Composable
fun HomeScreen(navigator: DestinationsNavigator) {
    val context = LocalContext.current
    val viewModel = viewModel<HomeViewModel>()
    val coroutineScope = rememberCoroutineScope()
    val loadingDialog = rememberLoadingDialog()

    LaunchedEffect(key1 = navigator) {
        viewModel.loadUserSettings(context)
        coroutineScope.launch {
            viewModel.loadCoreData()
            delay(100)
            viewModel.loadExtendedData(context)
        }

        // 启动数据变化监听（降低频率减少卡顿）
        coroutineScope.launch {
            while (true) {
                delay(15000) // 每15秒检查一次
                viewModel.autoRefreshIfNeeded(context)
            }
        }
    }

    LaunchedEffect(viewModel.dataRefreshTrigger) {
        viewModel.dataRefreshTrigger.collect { _ ->
            // 数据刷新时的额外处理可以在这里添加
        }
    }

    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())
    val scrollState = rememberScrollState()

    Scaffold(
        topBar = {
            TopBar(
                scrollBehavior = scrollBehavior,
                navigator = navigator,
                isDataLoaded = viewModel.isCoreDataLoaded
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        )
    ) { innerPadding ->
        YukiPullToRefreshBox(
            isRefreshing = viewModel.isRefreshing,
            onRefresh = { viewModel.onPullRefresh(context) },
            modifier = Modifier
                .padding(innerPadding)
                .fillMaxSize()
        ) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(scrollState)
                    .padding(top = 12.dp, start = 16.dp, end = 16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                // SuperKey 对话框
                val superKeyDialog = rememberSuperKeyDialog()
                var superKeyAuthSuccess by remember { mutableStateOf(false) }
                val snackbarHostState = remember { SnackbarHostState() }
                
                // 检查内核是否配置了 SuperKey / 签名（异步，避免阻塞主线程）
                val isSuperKeyConfigured by produceState(initialValue = false) {
                    value = withContext(Dispatchers.IO) { Natives.isSuperKeyConfigured() }
                }
                val isSignatureOk by produceState(initialValue = false) {
                    value = withContext(Dispatchers.IO) { Natives.isSignatureOk() }
                }
                val isLateLoadMode by produceState(
                    initialValue = false,
                    key1 = viewModel.systemStatus.ksuVersion
                ) {
                    value = withContext(Dispatchers.IO) {
                        if (viewModel.systemStatus.ksuVersion == null) {
                            false
                        } else {
                            runCatching { Natives.isLateLoadMode }.getOrDefault(false)
                        }
                    }
                }
                val superKeyPrefs = context.getSharedPreferences("superkey", Context.MODE_PRIVATE)
                
                SuperKeyDialog(
                    state = superKeyDialog,
                    onAuthenticate = { superKey ->
                        // 在 IO 线程执行 Native 调用，避免阻塞主线程
                        try {
                            kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                                val success = Natives.authenticateSuperKey(superKey)
                                if (success) {
                                    val skipStore = superKeyPrefs.getBoolean("skip_store_superkey", false)
                                    if (!skipStore) {
                                        superKeyPrefs.edit().putString("saved_superkey", superKey).apply()
                                    }
                                }
                                success
                            }
                        } catch (e: Exception) {
                            android.util.Log.e("SuperKey", "Authentication error", e)
                            false
                        }
                    },
                    onResult = { result ->
                        when (result) {
                            is SuperKeyAuthResult.Success -> {
                                superKeyAuthSuccess = true
                                // 强制刷新状态 - 认证成功后需要重新创建 Shell
                                coroutineScope.launch {
                                    // 等待内核状态更新
                                    delay(100)
                                    // 重新创建 Shell（之前的 Shell 没有 root 权限）
                                    withContext(Dispatchers.IO) {
                                        KsuCli.refreshShells()
                                    }
                                    // 强制刷新数据
                                    viewModel.refreshData(context, forceRefresh = true)
                                    withContext(Dispatchers.IO) {
                                        AppData.DataRefreshManager.refreshData()
                                    }
                                }
                            }
                            is SuperKeyAuthResult.Error -> {
                                coroutineScope.launch {
                                    snackbarHostState.showSnackbar(result.message)
                                }
                            }
                            SuperKeyAuthResult.Canceled -> {}
                        }
                    }
                )
                
                // 自动尝试用保存的 SuperKey 认证
                LaunchedEffect(viewModel.isCoreDataLoaded) {
                    if (viewModel.isCoreDataLoaded && !viewModel.systemStatus.isManager) {
                        val savedKey = superKeyPrefs.getString("saved_superkey", null)
                        if (!savedKey.isNullOrBlank()) {
                            try {
                                // 在 IO 线程执行 Native 调用和 Shell 刷新
                                val success = withContext(Dispatchers.IO) {
                                    val authSuccess = Natives.authenticateSuperKey(savedKey)
                                    if (authSuccess) {
                                        // 重新创建 Shell（之前的 Shell 没有 root 权限）
                                        KsuCli.refreshShells()
                                    }
                                    authSuccess
                                }
                                if (success) {
                                    superKeyAuthSuccess = true
                                    // 强制刷新数据
                                    viewModel.refreshData(context, forceRefresh = true)
                                    withContext(Dispatchers.IO) {
                                        AppData.DataRefreshManager.refreshData()
                                    }
                                }
                            } catch (e: Exception) {
                                android.util.Log.e("SuperKey", "Auto-auth error", e)
                            }
                        }
                    }
                }

                // 状态卡片
                if (viewModel.isCoreDataLoaded) {
                    val isNotManager = !viewModel.systemStatus.isManager
                    val needsSuperKeyAuth = isNotManager && !superKeyAuthSuccess && viewModel.systemStatus.ksuVersion == null
                    
                    StatusCard(
                        systemStatus = viewModel.systemStatus,
                        // SuperKey 模式用于表示「主要依赖 SuperKey」，
                        // 显示规则交给 StatusCard 内部根据 isSuperKeyMode + isSignatureOk 决定徽章组合。
                        isSuperKeyMode = isSuperKeyConfigured || superKeyAuthSuccess,
                        needsSuperKeyAuth = needsSuperKeyAuth,
                        onClickInstall = {
                            navigator.navigate(InstallScreenDestination())
                        },
                        onSuperKeyAuth = {
                            superKeyDialog.show()
                        },
                        isSignatureOk = isSignatureOk,
                        isLateLoadMode = isLateLoadMode,
                        canJailbreak = viewModel.systemStatus.ksuVersion == null &&
                            viewModel.systemInfo.seLinuxStatus == stringResource(R.string.selinux_status_permissive),
                        onJailbreak = {
                            loadingDialog.show()
                            if (!MagicaHelper.launch(context)) {
                                loadingDialog.hide()
                                Toast.makeText(context, R.string.home_jailbreak_failed, Toast.LENGTH_LONG).show()
                            } else {
                                coroutineScope.launch {
                                    delay(30_000)
                                    loadingDialog.hide()
                                    Toast.makeText(context, R.string.jailbreak_timeout, Toast.LENGTH_LONG).show()
                                }
                            }
                        }
                    )

                    // 警告信息
                    if (viewModel.systemStatus.requireNewKernel) {
                        WarningCard(
                            stringResource(id = R.string.require_kernel_version).format(
                                Natives.getSimpleVersionFull(),
                                Natives.MINIMAL_SUPPORTED_KERNEL_FULL
                            )
                        )
                    }

                    // UAPI 版本不匹配（管理器与内核 ABI 不同步）
                    if (viewModel.systemStatus.isManager &&
                        viewModel.systemStatus.ksuVersion != null &&
                        viewModel.systemStatus.kernelUapiVersion != viewModel.systemStatus.managerUapiVersion
                    ) {
                        WarningCard(
                            stringResource(
                                id = R.string.uapi_mismatch,
                                viewModel.systemStatus.managerUapiVersion,
                                viewModel.systemStatus.kernelUapiVersion
                            )
                        )
                    }

                    if (viewModel.systemStatus.ksuVersion != null && !viewModel.systemStatus.isRootAvailable) {
                        WarningCard(
                            stringResource(id = R.string.grant_root_failed)
                        )
                    }

                    // 只有在没有其他警告信息时才显示不兼容内核警告
                    val shouldShowWarnings = viewModel.systemStatus.requireNewKernel ||
                            (viewModel.systemStatus.ksuVersion != null && !viewModel.systemStatus.isRootAvailable)
                }

                if (viewModel.isExtendedDataLoaded) {
                    val checkUpdate = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
                        .getBoolean("check_update", true)
                    if (checkUpdate) {
                        UpdateCard()
                    }

                    // 信息卡片
                    InfoCard(
                        systemInfo = viewModel.systemInfo,
                        isSimpleMode = viewModel.isSimpleMode,
                        isHideZygiskImplement = viewModel.isHideZygiskImplement,
                        isHideMetaModuleImplement = viewModel.isHideMetaModuleImplement,
                        isHideSeccompStatus = viewModel.isHideSeccompStatus,
                        onYukiZygiskClick = { navigator.navigate(YukiZygiskScreenDestination) },
                    )

                    // 链接卡片
                    if (!viewModel.isSimpleMode && !viewModel.isHideLinkCard) {
                        ContributionCard()
                        DonateCard()
                    }
                }

                if (!viewModel.isExtendedDataLoaded) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(24.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        CircularProgressIndicator()
                    }
                }

                Spacer(Modifier.height(16.dp))
            }
        }
    }
}

@Composable
fun UpdateCard() {
    val context = LocalContext.current
    val latestVersionInfo = LatestVersionInfo()
    val newVersion by produceState(initialValue = latestVersionInfo) {
        value = withContext(Dispatchers.IO) {
            checkNewVersion()
        }
    }

    val currentVersionCode = getManagerVersion(context).second
    val newVersionCode = newVersion.versionCode
    val newVersionUrl = newVersion.downloadUrl
    val changelog = newVersion.changelog

    val uriHandler = LocalUriHandler.current
    val title = stringResource(id = R.string.module_changelog)
    val updateText = stringResource(id = R.string.module_update)

    AnimatedVisibility(
        visible = newVersionCode > currentVersionCode,
        enter = fadeIn() + expandVertically(
            animationSpec = spring(
                dampingRatio = Spring.DampingRatioMediumBouncy,
                stiffness = Spring.StiffnessLow
            )
        ),
        exit = shrinkVertically() + fadeOut()
    ) {
        val updateDialog = rememberConfirmDialog(onConfirm = { uriHandler.openUri(newVersionUrl) })
        WarningCard(
            message = stringResource(id = R.string.new_version_available).format(newVersionCode),
            color = MaterialTheme.colorScheme.outlineVariant,
            onClick = {
                if (changelog.isEmpty()) {
                    uriHandler.openUri(newVersionUrl)
                } else {
                    updateDialog.showConfirm(
                        title = title,
                        content = changelog,
                        markdown = true,
                        confirm = updateText
                    )
                }
            }
        )
    }
}

private data class RebootMenuOption(
    @param:StringRes val label: Int,
    val reason: String = "",
)

@Composable
private fun RebootDropdownItem(
    option: RebootMenuOption,
    index: Int,
    count: Int,
    onSelected: () -> Unit,
) {
    if (isExpressiveUi) {
        val shape = MenuDefaults.itemShape(index, count).shape
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = ListItemDefaults.SegmentedGap / 2)
                .defaultMinSize(minHeight = 48.dp)
                .clip(shape)
                .background(
                    MaterialTheme.colorScheme.surfaceContainer.copy(
                        alpha = CardConfig.cardAlpha
                    )
                )
                .clickable {
                    onSelected()
                    reboot(option.reason)
                }
                .padding(horizontal = 12.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = stringResource(option.label),
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Normal,
            )
        }
    } else {
        DropdownMenuItem(
            text = { Text(stringResource(option.label)) },
            onClick = {
                onSelected()
                reboot(option.reason)
            },
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    scrollBehavior: TopAppBarScrollBehavior? = null,
    navigator: DestinationsNavigator,
    isDataLoaded: Boolean = false
) {
    val context = LocalContext.current
    val colorScheme = MaterialTheme.colorScheme
    val snowflakeRotation = remember { Animatable(0f) }
    val cardColor = if (CardConfig.isCustomBackgroundEnabled) {
        colorScheme.surfaceContainerLow
    } else {
        colorScheme.background
    }

    LaunchedEffect(Unit) {
        snowflakeRotation.animateTo(
            targetValue = 360f,
            animationSpec = tween(
                durationMillis = 1200,
                easing = FastOutSlowInEasing,
            ),
        )
    }

    TopAppBar(
        title = {
            Icon(
                painter = painterResource(R.drawable.ic_launcher_monochrome),
                contentDescription = stringResource(R.string.app_name),
                tint = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier
                    .size(52.dp)
                    .graphicsLayer { rotationZ = snowflakeRotation.value },
            )
        },
        colors = TopAppBarDefaults.topAppBarColors(
            containerColor = cardColor,
            scrolledContainerColor = cardColor
        ),
        actions = {
            if (isDataLoaded) {
                // 重启按钮
                var showDropdown by remember { mutableStateOf(false) }
                KsuIsValid {
                    IconButton(onClick = {
                        showDropdown = true
                    }) {
                        YukiIcon(
                            imageVector = Icons.Filled.PowerSettingsNew,
                            contentDescription = stringResource(id = R.string.reboot)
                        )

                        val pm = LocalContext.current.getSystemService(
                            Context.POWER_SERVICE
                        ) as PowerManager?
                        val rebootOptions = buildList {
                            add(RebootMenuOption(R.string.reboot))
                            add(RebootMenuOption(R.string.reboot_soft, "soft_reboot"))
                            @Suppress("DEPRECATION")
                            if (
                                Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                                pm?.isRebootingUserspaceSupported == true
                            ) {
                                add(RebootMenuOption(R.string.reboot_userspace, "userspace"))
                            }
                            add(RebootMenuOption(R.string.reboot_recovery, "recovery"))
                            add(RebootMenuOption(R.string.reboot_bootloader, "bootloader"))
                            add(RebootMenuOption(R.string.reboot_download, "download"))
                            add(RebootMenuOption(R.string.reboot_edl, "edl"))
                        }

                        DropdownMenu(
                            expanded = showDropdown,
                            onDismissRequest = { showDropdown = false },
                            modifier = Modifier.clickHapticFeedback(),
                            shape = if (isExpressiveUi) RectangleShape else MenuDefaults.shape,
                            containerColor = if (isExpressiveUi) {
                                Color.Transparent
                            } else {
                                MenuDefaults.containerColor
                            },
                            tonalElevation = if (isExpressiveUi) 0.dp else MenuDefaults.TonalElevation,
                            shadowElevation = if (isExpressiveUi) 0.dp else MenuDefaults.ShadowElevation,
                        ) {
                            rebootOptions.forEachIndexed { index, option ->
                                RebootDropdownItem(
                                    option = option,
                                    index = index,
                                    count = rebootOptions.size,
                                    onSelected = { showDropdown = false },
                                )
                            }
                        }
                    }
                }
            }
        },
        windowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
        scrollBehavior = scrollBehavior
    )
}

@Composable
private fun StatusCard(
    systemStatus: HomeViewModel.SystemStatus,
    isSuperKeyMode: Boolean = false,
    needsSuperKeyAuth: Boolean = false,
    isSignatureOk: Boolean = false,
    isLateLoadMode: Boolean = false,
    canJailbreak: Boolean = false,
    onClickInstall: () -> Unit = {},
    onSuperKeyAuth: () -> Unit = {},
    onJailbreak: () -> Unit = {}
) {
    ElevatedCard(
        colors = getCardColors(
            when {
                systemStatus.ksuVersion != null -> MaterialTheme.colorScheme.secondaryContainer
                needsSuperKeyAuth -> MaterialTheme.colorScheme.tertiaryContainer
                else -> MaterialTheme.colorScheme.errorContainer
            }
        ),
        elevation = getCardElevation(),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable { onClickInstall() }
                .padding(24.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            when {
                systemStatus.ksuVersion != null -> {

                    val workingModeText = when {
                        Natives.isSafeMode -> stringResource(id = R.string.safe_mode)
                        else -> stringResource(id = R.string.home_working)
                    }

                    YukiIcon(
                        Icons.Outlined.TaskAlt,
                        contentDescription = stringResource(R.string.home_working),
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier
                            .size(28.dp)
                            .padding(
                                horizontal = 4.dp
                            ),
                    )

                    Column(Modifier.padding(start = 20.dp)) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text(
                                text = workingModeText,
                                style = MaterialTheme.typography.titleMedium,
                                color = MaterialTheme.colorScheme.primary,
                            )

                            Spacer(Modifier.width(8.dp))

                            // 认证模式标签：根据签名/SuperKey 状态组合显示
                            when {
                                // 签名 OK 且 SuperKey 已通过：两个徽章
                                isSignatureOk && isSuperKeyMode -> {
                                    Surface(
                                        shape = RoundedCornerShape(4.dp),
                                        color = MaterialTheme.colorScheme.secondary,
                                        modifier = Modifier
                                    ) {
                                        Text(
                                            text = stringResource(id = R.string.home_auth_signature_tag),
                                            style = MaterialTheme.typography.labelMedium,
                                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                            color = MaterialTheme.colorScheme.onSecondary
                                        )
                                    }
                                    Spacer(Modifier.width(6.dp))
                                    Surface(
                                        shape = RoundedCornerShape(4.dp),
                                        color = MaterialTheme.colorScheme.tertiary,
                                        modifier = Modifier
                                    ) {
                                        Text(
                                            text = stringResource(id = R.string.home_auth_superkey_tag),
                                            style = MaterialTheme.typography.labelMedium,
                                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                            color = MaterialTheme.colorScheme.onTertiary
                                        )
                                    }
                                    Spacer(Modifier.width(6.dp))
                                }
                                // 只有签名：仅 Signature 徽章
                                isSignatureOk && !isSuperKeyMode -> {
                                    Surface(
                                        shape = RoundedCornerShape(4.dp),
                                        color = MaterialTheme.colorScheme.secondary,
                                        modifier = Modifier
                                    ) {
                                        Text(
                                            text = stringResource(id = R.string.home_auth_signature_tag),
                                            style = MaterialTheme.typography.labelMedium,
                                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                            color = MaterialTheme.colorScheme.onSecondary
                                        )
                                    }
                                    Spacer(Modifier.width(6.dp))
                                }
                                // 签名未启用 / 失败，但 SuperKey 模式：仅 SuperKey 徽章
                                !isSignatureOk && isSuperKeyMode -> {
                                    Surface(
                                        shape = RoundedCornerShape(4.dp),
                                        color = MaterialTheme.colorScheme.tertiary,
                                        modifier = Modifier
                                    ) {
                                        Text(
                                            text = stringResource(id = R.string.home_auth_superkey_tag),
                                            style = MaterialTheme.typography.labelMedium,
                                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                            color = MaterialTheme.colorScheme.onTertiary
                                        )
                                    }
                                    Spacer(Modifier.width(6.dp))
                                }
                                // 其它情况（例如都没有）：不显示认证徽章
                            }

                            if (isLateLoadMode) {
                                Surface(
                                    shape = RoundedCornerShape(4.dp),
                                    color = MaterialTheme.colorScheme.tertiary,
                                    modifier = Modifier
                                ) {
                                    Text(
                                        text = stringResource(id = R.string.jailbreak_mode),
                                        style = MaterialTheme.typography.labelMedium,
                                        modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                        color = MaterialTheme.colorScheme.onTertiary
                                    )
                                }
                                Spacer(Modifier.width(6.dp))
                            }

                            // 架构标签（缓存避免重复 syscall）
                            val machine = remember { Os.uname().machine }
                            if (machine != "aarch64") {
                                Surface(
                                    shape = RoundedCornerShape(4.dp),
                                    color = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier
                                ) {
                                    Text(
                                        text = machine,
                                        style = MaterialTheme.typography.labelMedium,
                                        modifier = Modifier.padding(
                                            horizontal = 6.dp,
                                            vertical = 2.dp
                                        ),
                                        color = MaterialTheme.colorScheme.onPrimary
                                    )
                                }
                            }
                        }

                        val ctx = LocalContext.current
                        val isHideVersion by produceState(initialValue = false) {
                            value = withContext(Dispatchers.IO) {
                                ctx.getSharedPreferences("settings", Context.MODE_PRIVATE)
                                    .getBoolean("is_hide_version", false)
                            }
                        }

                        if (!isHideVersion) {
                            Spacer(Modifier.height(4.dp))
                            systemStatus.ksuFullVersion?.let {
                                // version_full (…@YukiSU) + (内核ksuver[/uapi]) in parens,
                                // matching the manager version's "(code/uapi)" style.
                                val ksuver = systemStatus.ksuVersion
                                val versionText = when {
                                    ksuver == null -> it
                                    systemStatus.kernelUapiVersion > 0 ->
                                        "$it ($ksuver/${systemStatus.kernelUapiVersion})"
                                    else -> "$it ($ksuver)"
                                }
                                Text(
                                    text = stringResource(R.string.home_working_version, versionText),
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.secondary,
                                )
                            }
                        }
                    }
                }

                // 需要 SuperKey 认证（未安装或未认证）
                needsSuperKeyAuth -> {
                    YukiIcon(
                        Icons.Outlined.Warning,
                        contentDescription = stringResource(R.string.home_not_installed),
                        tint = MaterialTheme.colorScheme.error,
                        modifier = Modifier
                            .size(28.dp)
                            .padding(horizontal = 4.dp),
                    )

                    Column(Modifier.padding(start = 20.dp).weight(1f)) {
                        Text(
                            text = stringResource(R.string.home_not_installed),
                            style = MaterialTheme.typography.titleMedium,
                            color = MaterialTheme.colorScheme.error
                        )

                        Spacer(Modifier.height(4.dp))
                        Text(
                            text = stringResource(R.string.home_click_to_install),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onErrorContainer
                        )
                    }
                    
                    // 超级密钥认证按钮
                    IconButton(
                        onClick = onSuperKeyAuth,
                        modifier = Modifier.size(40.dp)
                    ) {
                        YukiIcon(
                            imageVector = Icons.Default.Key,
                            contentDescription = stringResource(R.string.superkey_auth_title),
                            tint = MaterialTheme.colorScheme.tertiary
                        )
                    }
                }

                else -> {
                    YukiIcon(
                        Icons.Outlined.Warning,
                        contentDescription = stringResource(R.string.home_not_installed),
                        tint = MaterialTheme.colorScheme.error,
                        modifier = Modifier
                            .size(28.dp)
                            .padding(
                                horizontal = 4.dp
                            ),
                    )

                    Column(
                        Modifier
                            .padding(start = 20.dp)
                            .weight(1f)
                    ) {
                        Text(
                            text = stringResource(R.string.home_not_installed),
                            style = MaterialTheme.typography.titleMedium,
                            color = MaterialTheme.colorScheme.error
                        )

                        Spacer(Modifier.height(4.dp))
                        Text(
                            text = stringResource(R.string.home_click_to_install),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onErrorContainer
                        )
                    }

                    if (canJailbreak) {
                        Button(
                            onClick = onJailbreak,
                            colors = ButtonDefaults.buttonColors(
                                containerColor = MaterialTheme.colorScheme.error,
                                contentColor = MaterialTheme.colorScheme.onError
                            )
                        ) {
                            Text(stringResource(R.string.home_jailbreak))
                        }
                    }
                }

            }
        }
    }
}

@Composable
fun WarningCard(
    message: String,
    color: Color = MaterialTheme.colorScheme.error,
    onClick: (() -> Unit)? = null
) {
    ElevatedCard(
        colors = getCardColors(color),
        elevation = getCardElevation(),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .then(onClick?.let { Modifier.clickable { it() } } ?: Modifier)
                .padding(24.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = message,
                style = MaterialTheme.typography.bodyMedium,
            )
        }
    }
}

@Composable
private fun ExpressiveHomeTextGroup(
    title: String,
    content: String,
    onClick: () -> Unit,
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            text = title,
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Normal,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 8.dp),
        )
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = 56.dp)
                .clip(CardDefaults.elevatedShape)
                .background(
                    MaterialTheme.colorScheme.surfaceContainer.copy(
                        alpha = CardConfig.cardAlpha
                    )
                )
                .clickable(onClick = onClick)
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = content,
                style = MaterialTheme.typography.bodyMedium,
            )
        }
    }
}

@Composable
fun ContributionCard() {
    val uriHandler = LocalUriHandler.current
    val links = listOf("https://github.com/ShirkNeko", "https://github.com/udochina")
    val title = stringResource(R.string.home_ContributionCard_kernelsu)
    val content = stringResource(R.string.home_click_to_ContributionCard_kernelsu)
    val onClick = {
        val randomIndex = Random.nextInt(links.size)
        uriHandler.openUri(links[randomIndex])
    }

    if (isExpressiveUi) {
        ExpressiveHomeTextGroup(
            title = title,
            content = content,
            onClick = onClick,
        )
    } else {
        ElevatedCard(
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainer),
            elevation = getCardElevation(),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onClick)
                    .padding(24.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text(
                        text = title,
                        style = MaterialTheme.typography.titleSmall,
                    )

                    Spacer(Modifier.height(4.dp))
                    Text(
                        text = content,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        }
    }
}

@Composable
fun DonateCard() {
    val uriHandler = LocalUriHandler.current
    val title = stringResource(R.string.home_support_title)
    val content = stringResource(R.string.home_support_content)
    val onClick = { uriHandler.openUri("https://patreon.com/weishu") }

    if (isExpressiveUi) {
        ExpressiveHomeTextGroup(
            title = title,
            content = content,
            onClick = onClick,
        )
    } else {
        ElevatedCard(
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainer),
            elevation = getCardElevation(),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onClick)
                    .padding(24.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text(
                        text = title,
                        style = MaterialTheme.typography.titleSmall,
                    )

                    Spacer(Modifier.height(4.dp))
                    Text(
                        text = content,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        }
    }
}

@Composable
private fun HomeInfoItem(
    label: String,
    content: String,
    icon: ImageVector? = null,
    contentColor: Color = Color.Unspecified,
    onClick: (() -> Unit)? = null,
    trailing: @Composable (() -> Unit)? = null,
    index: Int = 0,
    count: Int = 1,
) {
    val expressive = isExpressiveUi
    val cardShape = CardDefaults.elevatedShape
    val compactShape = ListItemDefaults.shapes().shape
    val cardCorners = cardShape as? CornerBasedShape
    val compactCorners = compactShape as? CornerBasedShape
    val expressiveShape = when {
        count == 1 -> cardShape
        cardCorners == null || compactCorners == null -> compactShape
        index == 0 -> RoundedCornerShape(
            topStart = cardCorners.topStart,
            topEnd = cardCorners.topEnd,
            bottomEnd = compactCorners.bottomEnd,
            bottomStart = compactCorners.bottomStart,
        )
        index == count - 1 -> RoundedCornerShape(
            topStart = compactCorners.topStart,
            topEnd = compactCorners.topEnd,
            bottomEnd = cardCorners.bottomEnd,
            bottomStart = cardCorners.bottomStart,
        )
        else -> compactShape
    }
    Row(
        verticalAlignment = if (expressive) Alignment.CenterVertically else Alignment.Top,
        modifier = Modifier
            .fillMaxWidth()
            .then(
                if (expressive) {
                    Modifier
                        .padding(vertical = ListItemDefaults.SegmentedGap / 2)
                        .defaultMinSize(minHeight = 56.dp)
                        .clip(expressiveShape)
                        .background(
                            MaterialTheme.colorScheme.surfaceContainer.copy(
                                alpha = CardConfig.cardAlpha
                            )
                        )
                } else {
                    Modifier.padding(vertical = 8.dp)
                }
            )
            .then(
                if (onClick != null) {
                    Modifier.clickable(onClick = onClick)
                } else {
                    Modifier
                }
            )
            .then(
                if (expressive) {
                    Modifier.padding(horizontal = 16.dp, vertical = 6.dp)
                } else {
                    Modifier
                }
            )
    ) {
        if (icon != null) {
            YukiIcon(
                imageVector = icon,
                contentDescription = label,
                modifier = if (expressive) {
                    Modifier
                        .size(32.dp)
                        .padding(4.dp)
                } else {
                    Modifier
                        .size(28.dp)
                        .padding(vertical = 4.dp)
                },
            )
        }
        Spacer(modifier = Modifier.width(if (expressive) 12.dp else 16.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = label,
                style = MaterialTheme.typography.labelLarge,
                fontWeight = if (expressive) FontWeight.Normal else null,
            )
            Text(
                text = content,
                style = MaterialTheme.typography.bodyMedium,
                color = if (contentColor == Color.Unspecified) {
                    LocalContentColor.current
                } else {
                    contentColor
                },
                softWrap = true,
            )
        }
        trailing?.invoke()
    }
}

private data class HomeInfoEntry(
    val label: String,
    val content: String,
    val icon: ImageVector? = null,
    val contentColor: Color = Color.Unspecified,
    val onClick: (() -> Unit)? = null,
    val trailing: (@Composable () -> Unit)? = null,
)

@Composable
private fun InfoCard(
    systemInfo: HomeViewModel.SystemInfo,
    isSimpleMode: Boolean,
    isHideZygiskImplement: Boolean,
    isHideMetaModuleImplement: Boolean,
    isHideSeccompStatus: Boolean = false,
    onYukiZygiskClick: () -> Unit = {},
) {
    var showKsudDialog by remember { mutableStateOf(false) }
    var ksudApkVersion by remember { mutableStateOf<String?>(null) }
    var ksudInstalledVersion by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) {
        val (apk, installed) = withContext(Dispatchers.IO) {
            KsuCli.getKsudVersionsForUi()
        }
        ksudApkVersion = apk
        ksudInstalledVersion = installed
    }

    val ksudUnknown = stringResource(id = R.string.home_ksud_daemon_unknown)
    val apkVer = ksudApkVersion
    val installedVer = ksudInstalledVersion
    val hasMismatch = apkVer != null && installedVer != null && apkVer != installedVer
    val ksudContent = when {
        apkVer == null && installedVer == null -> ksudUnknown
        installedVer == null -> apkVer ?: ksudUnknown
        apkVer == null -> installedVer
        apkVer == installedVer -> apkVer
        else -> "$installedVer / APK: $apkVer"
    }
    val seccompText = when (systemInfo.seccompStatus) {
        -1 -> stringResource(R.string.seccomp_status_not_supported)
        0 -> stringResource(R.string.seccomp_status_disabled)
        1 -> stringResource(R.string.seccomp_status_strict)
        2 -> stringResource(R.string.seccomp_status_filter)
        else -> stringResource(R.string.seccomp_status_unknown)
    }
    val entries = buildList {
        add(HomeInfoEntry(
            label = stringResource(R.string.home_kernel),
            content = systemInfo.kernelRelease,
            icon = Icons.Default.Memory,
        ))
        if (!isSimpleMode) {
            add(HomeInfoEntry(
                label = stringResource(R.string.home_android_version),
                content = systemInfo.androidVersion,
                icon = Icons.Default.Android,
            ))
        }
        add(HomeInfoEntry(
            label = stringResource(R.string.home_device_model),
            content = systemInfo.deviceModel,
            icon = Icons.Default.PhoneAndroid,
        ))
        add(HomeInfoEntry(
            label = stringResource(R.string.home_manager_version),
            content = "${systemInfo.managerVersion.first} (${systemInfo.managerVersion.second.toInt()}/${Natives.getManagerUapiVersion()})",
            icon = Icons.Default.SettingsSuggest,
        ))
        add(HomeInfoEntry(
            label = stringResource(id = R.string.home_ksud_daemon_title),
            content = ksudContent,
            icon = Icons.Filled.Engineering,
            contentColor = if (hasMismatch) MaterialTheme.colorScheme.error else Color.Unspecified,
            onClick = { showKsudDialog = true },
        ))
        if (!isSimpleMode) {
            add(HomeInfoEntry(
                label = stringResource(R.string.home_hook_type),
                content = Natives.getHookType(),
                icon = Icons.Default.Link,
            ))
        }
        add(HomeInfoEntry(
            label = stringResource(R.string.home_selinux_status),
            content = systemInfo.seLinuxStatus,
            icon = Icons.Default.Security,
        ))
        if (!isHideSeccompStatus) {
            add(HomeInfoEntry(
                label = stringResource(R.string.home_seccomp_status),
                content = seccompText,
                icon = Icons.Default.LocalPolice,
            ))
        }
        if (!isHideZygiskImplement && !isSimpleMode && systemInfo.zygiskImplement != "None") {
            val isYukiZygisk = systemInfo.zygiskImplement == "YukiZygisk"
            add(HomeInfoEntry(
                label = stringResource(R.string.home_zygisk_implement),
                content = systemInfo.zygiskImplement,
                icon = Icons.Default.Adb,
                trailing = if (isYukiZygisk) {
                    {
                        YukiIcon(
                            imageVector = Icons.Filled.Build,
                            contentDescription = stringResource(R.string.settings_yukizygisk),
                            modifier = if (isExpressiveUi) {
                                Modifier
                                    .size(36.dp)
                                    .clickable(onClick = onYukiZygiskClick)
                                    .padding(4.dp)
                            } else {
                                Modifier
                                    .size(28.dp)
                                    .clickable(onClick = onYukiZygiskClick)
                                    .padding(vertical = 4.dp)
                            },
                        )
                    }
                } else null,
            ))
        }
        if (!isHideMetaModuleImplement && !isSimpleMode && systemInfo.metaModuleImplement != "None") {
            add(HomeInfoEntry(
                label = stringResource(R.string.home_meta_module_implement),
                content = systemInfo.metaModuleImplement,
                icon = Icons.Default.Extension,
            ))
        }
    }

    if (isExpressiveUi) {
        Column(modifier = Modifier.fillMaxWidth()) {
            entries.forEachIndexed { index, entry ->
                HomeInfoItem(
                    label = entry.label,
                    content = entry.content,
                    icon = entry.icon,
                    contentColor = entry.contentColor,
                    onClick = entry.onClick,
                    trailing = entry.trailing,
                    index = index,
                    count = entries.size,
                )
            }
        }
    } else {
        ElevatedCard(
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainer),
            elevation = getCardElevation(),
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 24.dp, top = 24.dp, end = 24.dp, bottom = 16.dp),
            ) {
                entries.forEach { entry ->
                    HomeInfoItem(
                        label = entry.label,
                        content = entry.content,
                        icon = entry.icon,
                        contentColor = entry.contentColor,
                        onClick = entry.onClick,
                        trailing = entry.trailing,
                    )
                }
            }
        }
    }

    if (showKsudDialog) {
        KsudVersionDialog(
            onDismiss = { showKsudDialog = false },
            onVersionsUpdated = { apk, installed ->
                ksudApkVersion = apk
                ksudInstalledVersion = installed
            }
        )
    }
}

@Composable
private fun KsudVersionDialog(
    onDismiss: () -> Unit,
    onVersionsUpdated: (apk: String?, installed: String?) -> Unit = { _, _ -> }
) {
    val scope = rememberCoroutineScope()
    val context = LocalContext.current

    var apkVersion by remember { mutableStateOf<String?>(null) }
    var installedVersion by remember { mutableStateOf<String?>(null) }
    var loading by remember { mutableStateOf(true) }
    var syncing by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        loading = true
        val (apk, installed) = KsuCli.getKsudVersionsForUi()
        apkVersion = apk
        installedVersion = installed
        loading = false
    }

    YukiAlertDialog(
        onDismissRequest = { if (!syncing) onDismiss() },
        title = { Text(stringResource(id = R.string.home_ksud_daemon_title)) },
        text = {
            if (loading) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 16.dp),
                    contentAlignment = Alignment.Center
                ) {
                    CircularProgressIndicator()
                }
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        text = stringResource(
                            id = R.string.home_ksud_daemon_apk_version,
                            apkVersion ?: context.getString(R.string.home_ksud_daemon_unknown)
                        ),
                        style = MaterialTheme.typography.bodyMedium
                    )
                    Text(
                        text = stringResource(
                            id = R.string.home_ksud_daemon_installed_version,
                            installedVersion
                                ?: context.getString(R.string.home_ksud_daemon_unknown)
                        ),
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { if (!syncing) onDismiss() }
            ) {
                Text(stringResource(id = R.string.close))
            }
        },
        dismissButton = {
            TextButton(
                enabled = !loading && !syncing,
                onClick = {
                    if (loading || syncing) return@TextButton
                    syncing = true
                    scope.launch {
                        KsuCli.updateKsudDaemonForUi()
                        val (apk, installed) = KsuCli.getKsudVersionsForUi()
                        apkVersion = apk
                        installedVersion = installed
                        onVersionsUpdated(apk, installed)
                        syncing = false
                    }
                }
            ) {
                Text(
                    text = if (syncing)
                        stringResource(id = R.string.home_ksud_daemon_syncing)
                    else
                        stringResource(id = R.string.home_ksud_daemon_sync)
                )
            }
        }
    )
}

fun getManagerVersion(context: Context): Pair<String, Long> {
    val packageInfo = context.packageManager.getPackageInfo(context.packageName, 0)!!
    val versionCode = PackageInfoCompat.getLongVersionCode(packageInfo)
    return Pair(packageInfo.versionName!!, versionCode)
}

@Preview
@Composable
private fun StatusCardPreview() {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        StatusCard(
            HomeViewModel.SystemStatus(
                isManager = true,
                ksuVersion = 1,
                kernelVersion = KernelVersion(5, 10, 101),
                isRootAvailable = true
            )
        )

        StatusCard(
            HomeViewModel.SystemStatus(
                isManager = true,
                ksuVersion = 10000,
                kernelVersion = KernelVersion(5, 10, 101),
                isRootAvailable = true
            )
        )

        StatusCard(
            HomeViewModel.SystemStatus(
                isManager = false,
                ksuVersion = null,
                kernelVersion = KernelVersion(5, 10, 101),
                isRootAvailable = false
            )
        )

        StatusCard(
            HomeViewModel.SystemStatus(
                isManager = false,
                ksuVersion = null,
                kernelVersion = KernelVersion(4, 10, 101),
                isRootAvailable = false
            )
        )
    }
}

@Composable
private fun IncompatibleKernelCard() {
    val currentKver = remember { Natives.version }
    val threshold   = Natives.MINIMAL_NEW_IOCTL_KERNEL

    val msg = stringResource(
        id = R.string.incompatible_kernel_msg,
        currentKver,
        threshold
    )

    WarningCard(
        message = msg,
        color = MaterialTheme.colorScheme.error
    )
}

@Preview
@Composable
private fun WarningCardPreview() {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        WarningCard(message = "Warning message")
        WarningCard(
            message = "Warning message ",
            MaterialTheme.colorScheme.outlineVariant,
            onClick = {})
    }
}
