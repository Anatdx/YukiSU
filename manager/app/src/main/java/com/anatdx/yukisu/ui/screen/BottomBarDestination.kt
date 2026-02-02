package com.anatdx.yukisu.ui.screen

import androidx.annotation.StringRes
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.outlined.*
import androidx.compose.ui.graphics.vector.ImageVector
import com.ramcosta.composedestinations.generated.destinations.*
import com.ramcosta.composedestinations.spec.DirectionDestinationSpec
import com.anatdx.yukisu.R

enum class BottomBarDestination(
    val direction: DirectionDestinationSpec,
    @param:StringRes val label: Int,
    val iconSelected: ImageVector,
    val iconNotSelected: ImageVector,
    val rootRequired: Boolean,
) {
    Home(HomeScreenDestination, R.string.home, Icons.Filled.Home, Icons.Outlined.Home, false),
    SuperUser(SuperUserScreenDestination, R.string.superuser, Icons.Filled.AdminPanelSettings, Icons.Outlined.AdminPanelSettings, true),
    Module(ModuleScreenDestination, R.string.module, Icons.Filled.Extension, Icons.Outlined.Extension, true),
    Settings(SettingScreenDestination, R.string.settings, Icons.Filled.Settings, Icons.Outlined.Settings, false),
}
