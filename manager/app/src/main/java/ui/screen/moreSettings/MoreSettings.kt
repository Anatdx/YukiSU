package ui.screen.moreSettings

import android.annotation.SuppressLint
import android.content.Context
import android.net.Uri
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.*
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.getValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import ui.screen.moreSettings.util.LocaleHelper
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.theme.component.ImageEditorDialog
import com.anatdx.yukisu.ui.component.KsuIsValid
import com.anatdx.yukisu.ui.screen.SwitchItem
import com.anatdx.yukisu.ui.theme.*
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import ui.screen.moreSettings.component.ColorCircle
import ui.screen.moreSettings.component.LanguageSelectionDialog
import ui.screen.moreSettings.component.MoreSettingsDialogs
import ui.screen.moreSettings.component.SettingItem
import ui.screen.moreSettings.component.SettingsCard
import ui.screen.moreSettings.component.SettingsDivider
import ui.screen.moreSettings.component.SwitchSettingItem
import ui.screen.moreSettings.state.MoreSettingsState
import kotlin.math.roundToInt

@SuppressLint("LocalContextConfigurationRead", "LocalContextResourcesRead", "ObsoleteSdkInt")
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun MoreSettingsScreen(
    navigator: DestinationsNavigator
) {

    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    val prefs = remember { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }
    val systemIsDark = isSystemInDarkTheme()


    val settingsState = remember { MoreSettingsState(context, prefs, systemIsDark) }
    val settingsHandlers = remember { MoreSettingsHandlers(context, prefs, settingsState) }


    val pickImageLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.GetContent()
    ) { uri: Uri? ->
        uri?.let {
            settingsState.selectedImageUri = it
            settingsState.showImageEditor = true
        }
    }


    LaunchedEffect(Unit) {
        settingsHandlers.initializeSettings()
    }


    if (settingsState.showImageEditor && settingsState.selectedImageUri != null) {
        ImageEditorDialog(
            imageUri = settingsState.selectedImageUri!!,
            onDismiss = {
                settingsState.showImageEditor = false
                settingsState.selectedImageUri = null
            },
            onConfirm = { transformedUri ->
                settingsHandlers.handleCustomBackground(transformedUri)
                settingsState.showImageEditor = false
                settingsState.selectedImageUri = null
            }
        )
    }


    MoreSettingsDialogs(
        state = settingsState,
        handlers = settingsHandlers
    )

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        text = stringResource(R.string.more_settings),
                        style = MaterialTheme.typography.titleLarge
                    )
                },
                navigationIcon = {
                    IconButton(onClick = { navigator.popBackStack() }) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = stringResource(R.string.back)
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surfaceContainerLow.copy(alpha = CardConfig.cardAlpha),
                    scrolledContainerColor = MaterialTheme.colorScheme.surfaceContainerLow.copy(alpha = CardConfig.cardAlpha)
                ),
                windowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
                scrollBehavior = scrollBehavior
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
    ) { paddingValues ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp)
                .padding(top = 8.dp)
        ) {

            AppearanceSettings(
                state = settingsState,
                handlers = settingsHandlers,
                pickImageLauncher = pickImageLauncher,
                coroutineScope = coroutineScope
            )


            CustomizationSettings(
                state = settingsState,
                handlers = settingsHandlers
            )


            KsuIsValid {
                AdvancedSettings(
                    state = settingsState,
                    handlers = settingsHandlers
                )
            }
        }
    }
}

@Composable
private fun AppearanceSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    pickImageLauncher: ActivityResultLauncher<String>,
    coroutineScope: CoroutineScope
) {
    SettingsCard(title = stringResource(R.string.appearance_settings)) {

        LanguageSetting(state = state)


        SettingItem(
            icon = Icons.Default.DarkMode,
            title = stringResource(R.string.theme_mode),
            subtitle = state.themeOptions[state.themeMode],
            onClick = { state.showThemeModeDialog = true }
        )


        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            SwitchSettingItem(
                icon = Icons.Filled.ColorLens,
                title = stringResource(R.string.dynamic_color_title),
                summary = stringResource(R.string.dynamic_color_summary),
                checked = state.useDynamicColor,
                onChange = handlers::handleDynamicColorChange
            )
        }


        AnimatedVisibility(
            visible = Build.VERSION.SDK_INT < Build.VERSION_CODES.S || !state.useDynamicColor,
            enter = fadeIn() + expandVertically(),
            exit = fadeOut() + shrinkVertically()
        ) {
            ThemeColorSelection(state = state)
        }

        SettingsDivider()


        DpiSettings(state = state, handlers = handlers)

        SettingsDivider()


        CustomBackgroundSettings(
            state = state,
            handlers = handlers,
            pickImageLauncher = pickImageLauncher,
            coroutineScope = coroutineScope
        )
    }
}

@Composable
private fun CustomizationSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingsCard(title = stringResource(R.string.custom_settings)) {

        SwitchSettingItem(
            icon = Icons.Filled.Info,
            title = stringResource(R.string.show_more_module_info),
            summary = stringResource(R.string.show_more_module_info_summary),
            checked = state.showMoreModuleInfo,
            onChange = handlers::handleShowMoreModuleInfoChange
        )


        SwitchSettingItem(
            icon = Icons.Filled.Brush,
            title = stringResource(R.string.simple_mode),
            summary = stringResource(R.string.simple_mode_summary),
            checked = state.isSimpleMode,
            onChange = handlers::handleSimpleModeChange
        )

        SwitchSettingItem(
            icon = Icons.Filled.Brush,
            title = stringResource(R.string.kernel_simple_kernel),
            summary = stringResource(R.string.kernel_simple_kernel_summary),
            checked = state.isKernelSimpleMode,
            onChange = handlers::handleKernelSimpleModeChange
        )


        HideOptionsSettings(state = state, handlers = handlers)
    }
}

@Composable
private fun HideOptionsSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {

    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_kernel_kernelsu_version),
        summary = stringResource(R.string.hide_kernel_kernelsu_version_summary),
        checked = state.isHideVersion,
        onChange = handlers::handleHideVersionChange
    )


    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_other_info),
        summary = stringResource(R.string.hide_other_info_summary),
        checked = state.isHideOtherInfo,
        onChange = handlers::handleHideOtherInfoChange
    )


    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_zygisk_implement),
        summary = stringResource(R.string.hide_zygisk_implement_summary),
        checked = state.isHideZygiskImplement,
        onChange = handlers::handleHideZygiskImplementChange
    )


    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_meta_module_implement),
        summary = stringResource(R.string.hide_meta_module_implement_summary),
        checked = state.isHideMetaModuleImplement,
        onChange = handlers::handleHideMetaModuleImplementChange
    )

    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_link_card),
        summary = stringResource(R.string.hide_link_card_summary),
        checked = state.isHideLinkCard,
        onChange = handlers::handleHideLinkCardChange
    )


    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_tag_card),
        summary = stringResource(R.string.hide_tag_card_summary),
        checked = state.isHideTagRow,
        onChange = handlers::handleHideTagRowChange
    )
}

@Composable
private fun AdvancedSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackBarHost = remember { SnackbarHostState() }
    val prefs = remember { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }

    SettingsCard(title = stringResource(R.string.advanced_settings)) {

        SwitchSettingItem(
            icon = Icons.Filled.Security,
            title = stringResource(R.string.selinux),
            summary = if (state.selinuxEnabled)
                stringResource(R.string.selinux_enabled) else
                stringResource(R.string.selinux_disabled),
            checked = state.selinuxEnabled,
            onChange = handlers::handleSelinuxChange
        )

        SwitchSettingItem(
            icon = Icons.Filled.Lock,
            title = stringResource(R.string.hide_bl_title),
            summary = if (state.hideBlEnabled)
                stringResource(R.string.hide_bl_enabled) else
                stringResource(R.string.hide_bl_disabled),
            checked = state.hideBlEnabled,
            onChange = handlers::handleHideBlChange
        )
    }
}

@Composable
private fun ThemeColorSelection(state: MoreSettingsState) {
    SettingItem(
        icon = Icons.Default.Palette,
        title = stringResource(R.string.theme_color),
        subtitle = when (ThemeConfig.currentTheme) {
            is ThemeColors.Green -> stringResource(R.string.color_green)
            is ThemeColors.Purple -> stringResource(R.string.color_purple)
            is ThemeColors.Orange -> stringResource(R.string.color_orange)
            is ThemeColors.Pink -> stringResource(R.string.color_pink)
            is ThemeColors.Gray -> stringResource(R.string.color_gray)
            is ThemeColors.Yellow -> stringResource(R.string.color_yellow)
            is ThemeColors.TransPride -> stringResource(R.string.color_trans_pride)  // 🏳️‍⚧️
            else -> stringResource(R.string.color_default)
        },
        onClick = { state.showThemeColorDialog = true },
        trailingContent = {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(start = 8.dp)
            ) {
                val theme = ThemeConfig.currentTheme
                val isDark = isSystemInDarkTheme()

                ColorCircle(
                    color = if (isDark) theme.primaryDark else theme.primaryLight,
                    isSelected = false,
                    modifier = Modifier.padding(horizontal = 2.dp)
                )
                ColorCircle(
                    color = if (isDark) theme.secondaryDark else theme.secondaryLight,
                    isSelected = false,
                    modifier = Modifier.padding(horizontal = 2.dp)
                )
                ColorCircle(
                    color = if (isDark) theme.tertiaryDark else theme.tertiaryLight,
                    isSelected = false,
                    modifier = Modifier.padding(horizontal = 2.dp)
                )
            }
        }
    )
}

@Composable
private fun DpiSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingItem(
        icon = Icons.Default.FormatSize,
        title = stringResource(R.string.app_dpi_title),
        subtitle = stringResource(R.string.app_dpi_summary),
        onClick = {},
        trailingContent = {
            Text(
                text = handlers.getDpiFriendlyName(state.tempDpi),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.primary
            )
        }
    )


    DpiSliderControls(state = state, handlers = handlers)
}

@Composable
private fun DpiSliderControls(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
        val sliderValue by animateFloatAsState(
            targetValue = state.tempDpi.toFloat(),
            label = "DPI Slider Animation"
        )

        Slider(
            value = sliderValue,
            onValueChange = { newValue ->
                state.tempDpi = newValue.toInt()
                state.isDpiCustom = !state.dpiPresets.containsValue(state.tempDpi)
            },
            valueRange = 160f..600f,
            steps = 11,
            colors = SliderDefaults.colors(
                thumbColor = MaterialTheme.colorScheme.primary,
                activeTrackColor = MaterialTheme.colorScheme.primary,
                inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant
            )
        )

        // DPI 预设按钮行
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp),
        ) {
            state.dpiPresets.forEach { (name, dpi) ->
                val isSelected = state.tempDpi == dpi
                val buttonColor = if (isSelected)
                    MaterialTheme.colorScheme.primaryContainer
                else
                    MaterialTheme.colorScheme.surfaceVariant

                Box(
                    modifier = Modifier
                        .weight(1f)
                        .padding(horizontal = 2.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(buttonColor)
                        .clickable {
                            state.tempDpi = dpi
                            state.isDpiCustom = false
                        }
                        .padding(vertical = 8.dp, horizontal = 4.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = name,
                        style = MaterialTheme.typography.labelMedium,
                        color = if (isSelected)
                            MaterialTheme.colorScheme.onPrimaryContainer
                        else
                            MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }

        Text(
            text = if (state.isDpiCustom)
                "${stringResource(R.string.dpi_size_custom)}: ${state.tempDpi}"
            else
                "${handlers.getDpiFriendlyName(state.tempDpi)}: ${state.tempDpi}",
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.padding(top = 8.dp)
        )

        Button(
            onClick = { state.showDpiConfirmDialog = true },
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp),
            enabled = state.tempDpi != state.currentDpi
        ) {
            Icon(
                Icons.Default.Check,
                contentDescription = null,
                modifier = Modifier.size(16.dp)
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text(stringResource(R.string.dpi_apply_settings))
        }
    }
}

@Composable
private fun CustomBackgroundSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    pickImageLauncher: ActivityResultLauncher<String>,
    coroutineScope: CoroutineScope
) {
    // 自定义背景开关
    SwitchSettingItem(
        icon = Icons.Filled.Wallpaper,
        title = stringResource(id = R.string.settings_custom_background),
        summary = stringResource(id = R.string.settings_custom_background_summary),
        checked = state.isCustomBackgroundEnabled,
        onChange = { isChecked ->
            if (isChecked) {
                pickImageLauncher.launch("image/*")
            } else {
                handlers.handleRemoveCustomBackground()
            }
        }
    )

    // 透明度和亮度调节
    AnimatedVisibility(
        visible = ThemeConfig.customBackgroundUri != null,
        enter = fadeIn() + slideInVertically(),
        exit = fadeOut() + slideOutVertically()
    ) {
        BackgroundAdjustmentControls(
            state = state,
            handlers = handlers,
            coroutineScope = coroutineScope
        )
    }
}

@Composable
private fun BackgroundAdjustmentControls(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    coroutineScope: CoroutineScope
) {
    Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
        // 透明度滑动条
        AlphaSlider(state = state, handlers = handlers, coroutineScope = coroutineScope)

        // 亮度调节滑动条
        DimSlider(state = state, handlers = handlers, coroutineScope = coroutineScope)
    }
}

@Composable
private fun AlphaSlider(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    coroutineScope: CoroutineScope
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(bottom = 4.dp)
    ) {
        Icon(
            Icons.Filled.Opacity,
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = stringResource(R.string.settings_card_alpha),
            style = MaterialTheme.typography.titleSmall
        )
        Spacer(modifier = Modifier.weight(1f))
        Text(
            text = "${(state.cardAlpha * 100).roundToInt()}%",
            style = MaterialTheme.typography.labelMedium,
        )
    }

    val alphaSliderValue by animateFloatAsState(
        targetValue = state.cardAlpha,
        label = "Alpha Slider Animation"
    )

    Slider(
        value = alphaSliderValue,
        onValueChange = { newValue ->
            handlers.handleCardAlphaChange(newValue)
        },
        onValueChangeFinished = {
            coroutineScope.launch(Dispatchers.IO) {
                saveCardConfig(handlers.context)
            }
        },
        valueRange = 0f..1f,
        steps = 20,
        colors = SliderDefaults.colors(
            thumbColor = MaterialTheme.colorScheme.primary,
            activeTrackColor = MaterialTheme.colorScheme.primary,
            inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant
        )
    )
}

@Composable
private fun DimSlider(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    coroutineScope: CoroutineScope
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(top = 16.dp, bottom = 4.dp)
    ) {
        Icon(
            Icons.Filled.LightMode,
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = stringResource(R.string.settings_card_dim),
            style = MaterialTheme.typography.titleSmall
        )
        Spacer(modifier = Modifier.weight(1f))
        Text(
            text = "${(state.cardDim * 100).roundToInt()}%",
            style = MaterialTheme.typography.labelMedium,
        )
    }

    val dimSliderValue by animateFloatAsState(
        targetValue = state.cardDim,
        label = "Dim Slider Animation"
    )

    Slider(
        value = dimSliderValue,
        onValueChange = { newValue ->
            handlers.handleCardDimChange(newValue)
        },
        onValueChangeFinished = {
            coroutineScope.launch(Dispatchers.IO) {
                saveCardConfig(handlers.context)
            }
        },
        valueRange = 0f..1f,
        steps = 20,
        colors = SliderDefaults.colors(
            thumbColor = MaterialTheme.colorScheme.primary,
            activeTrackColor = MaterialTheme.colorScheme.primary,
            inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant
        )
    )
}

fun saveCardConfig(context: Context) {
    CardConfig.save(context)
}

@Composable
private fun LanguageSetting(state: MoreSettingsState) {
    val context = LocalContext.current
    val language = stringResource(id = R.string.settings_language)

    // Compute display name based on current app locale
    val currentLanguageDisplay = remember(state.currentAppLocale) {
        val locale = state.currentAppLocale
        if (locale != null) {
            locale.getDisplayName(locale)
        } else {
            context.getString(R.string.language_system_default)
        }
    }

    SettingItem(
        icon = Icons.Filled.Translate,
        title = language,
        subtitle = currentLanguageDisplay,
        onClick = { state.showLanguageDialog = true }
    )

    // Language Selection Dialog
    if (state.showLanguageDialog) {
        LanguageSelectionDialog(
            onLanguageSelected = { newLocale ->
                // Update local state immediately
                state.currentAppLocale = LocaleHelper.getCurrentAppLocale(context)
                // Apply locale change immediately for Android < 13
                LocaleHelper.restartActivity(context)
            },
            onDismiss = { state.showLanguageDialog = false }
        )
    }
}