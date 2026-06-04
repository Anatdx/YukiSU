package com.anatdx.yukisu.ui.screen

import android.annotation.SuppressLint
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.*
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.expandVertically
import androidx.compose.animation.*
import androidx.compose.foundation.background
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.TopAppBarScrollBehavior
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import coil.compose.rememberAsyncImagePainter
import coil.request.ImageRequest
import com.dergoogler.mmrl.ui.component.LabelItem
import com.dergoogler.mmrl.ui.component.LabelItemDefaults
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.AppProfileScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.FabMenuPresets
import com.anatdx.yukisu.ui.component.SearchAppBar
import com.anatdx.yukisu.ui.component.VerticalExpandableFab
import com.anatdx.yukisu.ui.util.module.ModuleModify
import com.anatdx.yukisu.ui.viewmodel.AppCategory
import com.anatdx.yukisu.ui.viewmodel.SortType
import com.anatdx.yukisu.ui.viewmodel.SuperUserViewModel
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import kotlin.math.abs
import kotlin.math.roundToInt

enum class AppPriority(val value: Int) {
    ROOT(1), CUSTOM(2), DEFAULT(3)
}

data class BottomSheetMenuItem(
    val icon: ImageVector,
    val titleRes: Int,
    val onClick: () -> Unit
)

private enum class AppSwipeAction {
    GrantRoot,
    UmountModules
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun SuperUserScreen(navigator: DestinationsNavigator) {
    val viewModel = viewModel<SuperUserViewModel>()
    val scope = rememberCoroutineScope()
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())
    val listState = rememberLazyListState()
    val context = LocalContext.current
    val snackBarHostState = remember { SnackbarHostState() }

    val bottomSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    var showBottomSheet by remember { mutableStateOf(false) }

    val backupLauncher = ModuleModify.rememberAllowlistBackupLauncher(context, snackBarHostState)
    val restoreLauncher = ModuleModify.rememberAllowlistRestoreLauncher(context, snackBarHostState)

    LaunchedEffect(navigator) {
        viewModel.search = ""
    }

    LaunchedEffect(viewModel.selectedApps, viewModel.showBatchActions) {
        if (viewModel.showBatchActions && viewModel.selectedApps.isEmpty()) {
            viewModel.showBatchActions = false
        }
    }

    val filteredAndSortedAppGroups = remember(
        viewModel.appGroupList,
        viewModel.selectedCategory,
        viewModel.currentSortType,
        viewModel.search,
        viewModel.showSystemApps
    ) {
        var groups = viewModel.appGroupList

        // 按分类筛选
        groups = when (viewModel.selectedCategory) {
            AppCategory.ALL -> groups
            AppCategory.ROOT -> groups.filter { it.allowSu }
            AppCategory.CUSTOM -> groups.filter { !it.allowSu && it.hasCustomProfile }
            AppCategory.DEFAULT -> groups.filter { !it.allowSu && !it.hasCustomProfile }
        }

        // 排序
        groups.sortedWith { group1, group2 ->
            val priority1 = when {
                group1.allowSu -> AppPriority.ROOT
                group1.hasCustomProfile -> AppPriority.CUSTOM
                else -> AppPriority.DEFAULT
            }
            val priority2 = when {
                group2.allowSu -> AppPriority.ROOT
                group2.hasCustomProfile -> AppPriority.CUSTOM
                else -> AppPriority.DEFAULT
            }

            val priorityComparison = priority1.value.compareTo(priority2.value)
            if (priorityComparison != 0) {
                priorityComparison
            } else {
                when (viewModel.currentSortType) {
                    SortType.NAME_ASC -> group1.mainApp.label.lowercase()
                        .compareTo(group2.mainApp.label.lowercase())
                    SortType.NAME_DESC -> group2.mainApp.label.lowercase()
                        .compareTo(group1.mainApp.label.lowercase())
                    SortType.INSTALL_TIME_NEW -> group2.mainApp.packageInfo.firstInstallTime
                        .compareTo(group1.mainApp.packageInfo.firstInstallTime)
                    SortType.INSTALL_TIME_OLD -> group1.mainApp.packageInfo.firstInstallTime
                        .compareTo(group2.mainApp.packageInfo.firstInstallTime)
                    else -> group1.mainApp.label.lowercase()
                        .compareTo(group2.mainApp.label.lowercase())
                }
            }
        }
    }

    val appCounts = remember(viewModel.appGroupList, viewModel.showSystemApps) {
        mapOf(
            AppCategory.ALL to viewModel.appGroupList.size,
            AppCategory.ROOT to viewModel.appGroupList.count { it.allowSu },
            AppCategory.CUSTOM to viewModel.appGroupList.count { !it.allowSu && it.hasCustomProfile },
            AppCategory.DEFAULT to viewModel.appGroupList.count { !it.allowSu && !it.hasCustomProfile }
        )
    }

    Scaffold(
        topBar = {
            SearchAppBar(
                title = { TopBarTitle(viewModel.selectedCategory, appCounts) },
                searchText = viewModel.search,
                onSearchTextChange = { viewModel.search = it },
                onClearClick = { viewModel.search = "" },
                dropdownContent = {
                    IconButton(onClick = { showBottomSheet = true }) {
                        Icon(
                            imageVector = Icons.Filled.MoreVert,
                            contentDescription = stringResource(id = R.string.settings),
                        )
                    }
                },
                scrollBehavior = scrollBehavior
            )
        },
        snackbarHost = { SnackbarHost(snackBarHostState) },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
        floatingActionButton = {
            SuperUserFab(viewModel, filteredAndSortedAppGroups, listState, scope)
        }
    ) { innerPadding ->
        SuperUserContent(
            innerPadding = innerPadding,
            viewModel = viewModel,
            filteredAndSortedAppGroups = filteredAndSortedAppGroups,
            listState = listState,
            scrollBehavior = scrollBehavior,
            navigator = navigator,
            scope = scope
        )

        if (showBottomSheet) {
            SuperUserBottomSheet(
                bottomSheetState = bottomSheetState,
                onDismiss = { showBottomSheet = false },
                viewModel = viewModel,
                appCounts = appCounts,
                backupLauncher = backupLauncher,
                restoreLauncher = restoreLauncher,
                scope = scope,
                listState = listState
            )
        }
    }
}

@Composable
private fun TopBarTitle(
    selectedCategory: AppCategory,
    appCounts: Map<AppCategory, Int>
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(stringResource(R.string.superuser))

        if (selectedCategory != AppCategory.ALL) {
            Surface(
                shape = RoundedCornerShape(12.dp),
                color = MaterialTheme.colorScheme.primaryContainer,
                modifier = Modifier.padding(start = 4.dp)
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    Text(
                        text = stringResource(selectedCategory.displayNameRes),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onPrimaryContainer
                    )
                    Text(
                        text = "(${appCounts[selectedCategory] ?: 0})",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onPrimaryContainer
                    )
                }
            }
        }
    }
}

@Composable
private fun SuperUserFab(
    viewModel: SuperUserViewModel,
    filteredAndSortedAppGroups: List<SuperUserViewModel.AppGroup>,
    listState: androidx.compose.foundation.lazy.LazyListState,
    scope: CoroutineScope
) {
    VerticalExpandableFab(
        menuItems = if (viewModel.showBatchActions && viewModel.selectedApps.isNotEmpty()) {
            FabMenuPresets.getBatchActionMenuItems(
                onCancel = {
                    viewModel.selectedApps = emptySet()
                    viewModel.showBatchActions = false
                },
                onDeny = { scope.launch { viewModel.updateBatchPermissions(false) } },
                onAllow = { scope.launch { viewModel.updateBatchPermissions(true) } },
                onUnmountModules = {
                    scope.launch { viewModel.updateBatchPermissions(
                        allowSu = false,
                        umountModules = true
                    ) }
                },
                onDisableUnmount = {
                    scope.launch { viewModel.updateBatchPermissions(
                        allowSu = false,
                        umountModules = false
                    ) }
                }
            )
        } else {
            FabMenuPresets.getScrollMenuItems(
                onScrollToTop = { scope.launch { listState.animateScrollToItem(0) } },
                onScrollToBottom = {
                    scope.launch {
                        val lastIndex = filteredAndSortedAppGroups.size - 1
                        if (lastIndex >= 0) listState.animateScrollToItem(lastIndex)
                    }
                }
            )
        },
        mainButtonIcon = if (viewModel.showBatchActions && viewModel.selectedApps.isNotEmpty()) {
            Icons.Filled.GridView
        } else {
            Icons.Filled.Add
        },
        mainButtonExpandedIcon = Icons.Filled.Close
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SuperUserContent(
    innerPadding: PaddingValues,
    viewModel: SuperUserViewModel,
    filteredAndSortedAppGroups: List<SuperUserViewModel.AppGroup>,
    listState: androidx.compose.foundation.lazy.LazyListState,
    scrollBehavior: TopAppBarScrollBehavior,
    navigator: DestinationsNavigator,
    scope: CoroutineScope
) {
    val expandedGroups = remember { mutableStateOf(setOf<Int>()) }
    val density = LocalDensity.current
    val targetSizePx = remember(density) { with(density) { 36.dp.roundToPx() } }
    val context = LocalContext.current

    PullToRefreshBox(
        modifier = Modifier.padding(innerPadding),
        onRefresh = { scope.launch { viewModel.fetchAppList() } },
        isRefreshing = viewModel.isRefreshing
    ) {
        LazyColumn(
            state = listState,
            modifier = Modifier
                .fillMaxSize()
                .nestedScroll(scrollBehavior.nestedScrollConnection)
        ) {
            filteredAndSortedAppGroups.forEachIndexed { _, appGroup ->
                item(key = "${appGroup.uid}-${appGroup.mainApp.packageName}") {
                    AppGroupItem(
                        expandedGroups = expandedGroups,
                        appGroup = appGroup,
                        isSelected = appGroup.packageNames.any { viewModel.selectedApps.contains(it) },
                        onToggleSelection = {
                            appGroup.packageNames.forEach { viewModel.toggleAppSelection(it) }
                        },
                        onClick = {
                            if (viewModel.showBatchActions) {
                                appGroup.packageNames.forEach { viewModel.toggleAppSelection(it) }
                            } else if (appGroup.apps.size > 1) {
                                expandedGroups.value = if (expandedGroups.value.contains(appGroup.uid)) {
                                    expandedGroups.value - appGroup.uid
                                } else {
                                    expandedGroups.value + appGroup.uid
                                }
                            } else {
                                navigator.navigate(AppProfileScreenDestination(appGroup.mainApp))
                            }
                        },
                        onLongClick = {
                            if (!viewModel.showBatchActions) {
                                viewModel.toggleBatchMode()
                                appGroup.packageNames.forEach { viewModel.toggleAppSelection(it) }
                            }
                        },
                        viewModel = viewModel
                    )
                }

                if (appGroup.apps.size <= 1) return@forEachIndexed

                items(appGroup.apps, key = { "${it.packageName}-${it.uid}" }) { app ->
                    val painter = rememberAsyncImagePainter(
                        model = ImageRequest.Builder(context)
                            .data(app.packageInfo)
                            .size(targetSizePx)
                            .crossfade(true)
                            .build()
                    )

                    val listItemContent = remember(app.packageName, appGroup.uid) {
                        @Composable {
                            ListItem(
                                modifier = Modifier
                                    .clickable { navigator.navigate(AppProfileScreenDestination(app)) }
                                    .fillMaxWidth()
                                    .padding(start = 10.dp),
                                headlineContent = { Text(app.label, style = MaterialTheme.typography.bodyMedium) },
                                supportingContent = { Text(app.packageName, style = MaterialTheme.typography.bodySmall) },
                                leadingContent = {
                                    Image(
                                        painter = painter,
                                        contentDescription = app.label,
                                        modifier = Modifier
                                            .padding(4.dp)
                                            .size(36.dp),
                                        contentScale = ContentScale.Crop
                                    )
                                }
                            )
                        }
                    }

                    AnimatedVisibility(
                        visible = expandedGroups.value.contains(appGroup.uid),
                        enter = fadeIn() + expandVertically(),
                        exit = fadeOut() + shrinkVertically()
                    ) {
                        listItemContent()
                    }
                }
            }

            if (filteredAndSortedAppGroups.isEmpty()) {
                item {
                    Box(
                        modifier = Modifier.fillMaxWidth().height(400.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        if ((viewModel.isRefreshing || viewModel.appGroupList.isEmpty()) && viewModel.search.isEmpty()) {
                            LoadingAnimation(isLoading = true)
                        } else {
                            EmptyState(
                                selectedCategory = viewModel.selectedCategory,
                                isSearchEmpty = viewModel.search.isNotEmpty()
                            )
                        }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SuperUserBottomSheet(
    bottomSheetState: SheetState,
    onDismiss: () -> Unit,
    viewModel: SuperUserViewModel,
    appCounts: Map<AppCategory, Int>,
    backupLauncher: androidx.activity.result.ActivityResultLauncher<android.content.Intent>,
    restoreLauncher: androidx.activity.result.ActivityResultLauncher<android.content.Intent>,
    scope: CoroutineScope,
    listState: androidx.compose.foundation.lazy.LazyListState
) {
    val bottomSheetMenuItems = remember(viewModel.showSystemApps) {
        listOf(
            BottomSheetMenuItem(
                icon = Icons.Filled.Refresh,
                titleRes = R.string.refresh,
                onClick = {
                    scope.launch {
                        viewModel.fetchAppList()
                        bottomSheetState.hide()
                        onDismiss()
                    }
                }
            ),
            BottomSheetMenuItem(
                icon = if (viewModel.showSystemApps) Icons.Filled.VisibilityOff else Icons.Filled.Visibility,
                titleRes = if (viewModel.showSystemApps) R.string.hide_system_apps else R.string.show_system_apps,
                onClick = {
                    viewModel.updateShowSystemApps(!viewModel.showSystemApps)
                    scope.launch {
                        kotlinx.coroutines.delay(100)
                        bottomSheetState.hide()
                        onDismiss()
                    }
                }
            ),
            BottomSheetMenuItem(
                icon = Icons.Filled.Save,
                titleRes = R.string.backup_allowlist,
                onClick = {
                    backupLauncher.launch(ModuleModify.createAllowlistBackupIntent())
                    scope.launch {
                        bottomSheetState.hide()
                        onDismiss()
                    }
                }
            ),
            BottomSheetMenuItem(
                icon = Icons.Filled.RestoreFromTrash,
                titleRes = R.string.restore_allowlist,
                onClick = {
                    restoreLauncher.launch(ModuleModify.createAllowlistRestoreIntent())
                    scope.launch {
                        bottomSheetState.hide()
                        onDismiss()
                    }
                }
            )
        )
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = bottomSheetState,
        dragHandle = {
            Surface(
                modifier = Modifier.padding(vertical = 11.dp),
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                shape = RoundedCornerShape(16.dp)
            ) {
                Box(Modifier.size(width = 32.dp, height = 4.dp))
            }
        }
    ) {
        BottomSheetContent(
            menuItems = bottomSheetMenuItems,
            currentSortType = viewModel.currentSortType,
            onSortTypeChanged = { newSortType ->
                viewModel.updateCurrentSortType(newSortType)
                scope.launch {
                    bottomSheetState.hide()
                    onDismiss()
                }
            },
            selectedCategory = viewModel.selectedCategory,
            onCategorySelected = { newCategory ->
                viewModel.updateSelectedCategory(newCategory)
                scope.launch {
                    listState.animateScrollToItem(0)
                    bottomSheetState.hide()
                    onDismiss()
                }
            },
            appCounts = appCounts
        )
    }
}

@Composable
private fun BottomSheetContent(
    menuItems: List<BottomSheetMenuItem>,
    currentSortType: SortType,
    onSortTypeChanged: (SortType) -> Unit,
    selectedCategory: AppCategory,
    onCategorySelected: (AppCategory) -> Unit,
    appCounts: Map<AppCategory, Int>
) {
    Column(
        modifier = Modifier.fillMaxWidth().padding(bottom = 24.dp)
    ) {
        Text(
            text = stringResource(R.string.menu_options),
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 24.dp, vertical = 16.dp)
        )

        LazyVerticalGrid(
            columns = GridCells.Fixed(4),
            modifier = Modifier.fillMaxWidth(),
            contentPadding = PaddingValues(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            items(menuItems) { menuItem ->
                BottomSheetMenuItemView(menuItem = menuItem)
            }
        }

        Spacer(modifier = Modifier.height(24.dp))
        HorizontalDivider(modifier = Modifier.padding(horizontal = 24.dp))

        Text(
            text = stringResource(R.string.sort_options),
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 24.dp, vertical = 16.dp)
        )

        LazyRow(
            modifier = Modifier.fillMaxWidth(),
            contentPadding = PaddingValues(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(SortType.entries.toTypedArray()) { sortType ->
                FilterChip(
                    onClick = { onSortTypeChanged(sortType) },
                    label = { Text(stringResource(sortType.displayNameRes)) },
                    selected = currentSortType == sortType
                )
            }
        }

        Spacer(modifier = Modifier.height(24.dp))
        HorizontalDivider(modifier = Modifier.padding(horizontal = 24.dp))

        Text(
            text = stringResource(R.string.app_categories),
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 24.dp, vertical = 16.dp)
        )

        LazyVerticalGrid(
            columns = GridCells.Fixed(2),
            modifier = Modifier.fillMaxWidth(),
            contentPadding = PaddingValues(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(AppCategory.entries.toTypedArray()) { category ->
                CategoryChip(
                    category = category,
                    isSelected = selectedCategory == category,
                    onClick = { onCategorySelected(category) },
                    appCount = appCounts[category] ?: 0
                )
            }
        }
    }
}

@Composable
private fun CategoryChip(
    category: AppCategory,
    isSelected: Boolean,
    onClick: () -> Unit,
    appCount: Int,
    modifier: Modifier = Modifier
) {
    val interactionSource = remember { MutableInteractionSource() }
    val isPressed by interactionSource.collectIsPressedAsState()

    val scale by animateFloatAsState(
        targetValue = if (isPressed) 0.95f else 1.0f,
        animationSpec = spring(
            dampingRatio = Spring.DampingRatioMediumBouncy,
            stiffness = Spring.StiffnessHigh
        ),
        label = "categoryChipScale"
    )

    Surface(
        modifier = modifier
            .fillMaxWidth()
            .scale(scale)
            .clickable(interactionSource = interactionSource, indication = null) { onClick() },
        shape = RoundedCornerShape(12.dp),
        color = if (isSelected) {
            MaterialTheme.colorScheme.primaryContainer
        } else {
            MaterialTheme.colorScheme.surfaceVariant
        },
        tonalElevation = if (isSelected) 4.dp else 0.dp
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Text(
                    text = stringResource(category.displayNameRes),
                    style = MaterialTheme.typography.titleSmall.copy(
                        fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Medium
                    ),
                    color = if (isSelected) {
                        MaterialTheme.colorScheme.onPrimaryContainer
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )

                AnimatedVisibility(
                    visible = isSelected,
                    enter = scaleIn() + fadeIn(),
                    exit = scaleOut() + fadeOut()
                ) {
                    Icon(
                        imageVector = Icons.Filled.Check,
                        contentDescription = stringResource(R.string.selected),
                        tint = MaterialTheme.colorScheme.onPrimaryContainer,
                        modifier = Modifier.size(16.dp)
                    )
                }
            }

            Text(
                text = "$appCount apps",
                style = MaterialTheme.typography.labelSmall,
                color = if (isSelected) {
                    MaterialTheme.colorScheme.onPrimaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                }
            )
        }
    }
}

@Composable
private fun BottomSheetMenuItemView(menuItem: BottomSheetMenuItem) {
    val interactionSource = remember { MutableInteractionSource() }
    val isPressed by interactionSource.collectIsPressedAsState()

    val scale by animateFloatAsState(
        targetValue = if (isPressed) 0.95f else 1.0f,
        animationSpec = spring(
            dampingRatio = Spring.DampingRatioMediumBouncy,
            stiffness = Spring.StiffnessHigh
        ),
        label = "menuItemScale"
    )

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .scale(scale)
            .clickable(interactionSource = interactionSource, indication = null) { menuItem.onClick() }
            .padding(8.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Surface(
            modifier = Modifier.size(48.dp),
            shape = CircleShape,
            color = MaterialTheme.colorScheme.primaryContainer,
            contentColor = MaterialTheme.colorScheme.onPrimaryContainer
        ) {
            Box(contentAlignment = Alignment.Center) {
                Icon(
                    imageVector = menuItem.icon,
                    contentDescription = stringResource(menuItem.titleRes),
                    modifier = Modifier.size(24.dp)
                )
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        Text(
            text = stringResource(menuItem.titleRes),
            style = MaterialTheme.typography.labelSmall,
            textAlign = TextAlign.Center,
            maxLines = 2
        )
    }
}

@Composable
private fun LoadingAnimation(
    modifier: Modifier = Modifier,
    isLoading: Boolean = true
) {
    val infiniteTransition = rememberInfiniteTransition(label = "loading")

    val alpha by infiniteTransition.animateFloat(
        initialValue = 0.3f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(
            animation = tween(600, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Reverse
        ),
        label = "alpha"
    )

    AnimatedVisibility(
        visible = isLoading,
        enter = fadeIn() + scaleIn(),
        exit = fadeOut() + scaleOut(),
        modifier = modifier
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            LinearProgressIndicator(
                modifier = Modifier.width(200.dp).height(4.dp),
                color = MaterialTheme.colorScheme.primary.copy(alpha = alpha),
                trackColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.2f)
            )
        }
    }
}

@Composable
@SuppressLint("ModifierParameter")
private fun EmptyState(
    selectedCategory: AppCategory,
    modifier: Modifier = Modifier,
    isSearchEmpty: Boolean = false
) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
        modifier = modifier
    ) {
        Icon(
            imageVector = if (isSearchEmpty) Icons.Filled.SearchOff else Icons.Filled.Archive,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f),
            modifier = Modifier.size(96.dp).padding(bottom = 16.dp)
        )
        Text(
            text = if (isSearchEmpty || selectedCategory == AppCategory.ALL) {
                stringResource(R.string.no_apps_found)
            } else {
                stringResource(R.string.no_apps_in_category)
            },
            textAlign = TextAlign.Center,
            style = MaterialTheme.typography.bodyLarge,
        )
    }
}

@Composable
private fun SwipeActionContainer(
    enabled: Boolean,
    onSwipeLeft: suspend () -> Boolean,
    onSwipeRight: suspend () -> Boolean,
    content: @Composable () -> Unit
) {
    val density = LocalDensity.current
    val hapticFeedback = LocalHapticFeedback.current
    val actionThresholdPx = remember(density) { with(density) { 132.dp.toPx() } }
    val maxRevealPx = remember(density) { with(density) { 148.dp.toPx() } }
    val rootBackgroundColor = Color(0xFFDFF3D8)
    val rootContentColor = Color(0xFF246B35)
    val umountBackgroundColor = Color(0xFFFFE1E1)
    val umountContentColor = Color(0xFF9B1C1C)
    val scope = rememberCoroutineScope()
    var targetOffsetX by remember { mutableFloatStateOf(0f) }
    var isDragging by remember { mutableStateOf(false) }
    var settleSpec by remember { mutableStateOf<AnimationSpec<Float>>(spring(stiffness = Spring.StiffnessMediumLow)) }
    val offsetX by animateFloatAsState(
        targetValue = targetOffsetX,
        animationSpec = if (isDragging) snap() else settleSpec,
        label = "appSwipeOffset"
    )
    val action = when {
        offsetX < 0f -> AppSwipeAction.UmountModules
        offsetX > 0f -> AppSwipeAction.GrantRoot
        else -> null
    }
    val backgroundColor = when (action) {
        AppSwipeAction.GrantRoot -> rootBackgroundColor
        AppSwipeAction.UmountModules -> umountBackgroundColor
        null -> Color.Transparent
    }
    val contentColor = when (action) {
        AppSwipeAction.GrantRoot -> rootContentColor
        AppSwipeAction.UmountModules -> umountContentColor
        null -> Color.Transparent
    }
    val actionProgress = (abs(offsetX) / actionThresholdPx).coerceIn(0f, 1f)

    fun boundedSwipeOffset(rawOffset: Float): Float {
        return rawOffset.coerceIn(-maxRevealPx, maxRevealPx)
    }

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(0.dp))
            .background(backgroundColor)
    ) {
        if (action != null) {
            SwipeActionHint(
                action = action,
                contentColor = contentColor.copy(alpha = 0.45f + actionProgress * 0.55f),
                modifier = Modifier
                    .matchParentSize()
                    .padding(horizontal = 24.dp)
            )
        }

        Box(
            modifier = Modifier
                .offset { IntOffset(offsetX.roundToInt(), 0) }
                .fillMaxWidth()
                .pointerInput(enabled, onSwipeLeft, onSwipeRight) {
                    if (!enabled) return@pointerInput

                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        var pointerId = down.id
                        var totalX = 0f
                        var totalY = 0f
                        var horizontalLocked = false
                        var thresholdHapticSent = false

                        while (!horizontalLocked) {
                            val event = awaitPointerEvent(PointerEventPass.Main)
                            val change = event.changes.firstOrNull { it.id == pointerId }
                                ?: event.changes.firstOrNull { it.pressed }
                                ?: return@awaitEachGesture

                            pointerId = change.id

                            if (!change.pressed) {
                                return@awaitEachGesture
                            }

                            val delta = change.positionChange()
                            totalX += delta.x
                            totalY += delta.y

                            val absX = abs(totalX)
                            val absY = abs(totalY)
                            val touchSlop = viewConfiguration.touchSlop

                            if (absY > touchSlop && absY > absX) {
                                return@awaitEachGesture
                            }

                            if (absX > touchSlop * 2.4f && absX > absY * 1.8f) {
                                horizontalLocked = true
                                change.consume()
                                isDragging = true
                                targetOffsetX = boundedSwipeOffset(totalX)
                            }
                        }

                        while (true) {
                            val event = awaitPointerEvent(PointerEventPass.Main)
                            val change = event.changes.firstOrNull { it.id == pointerId }
                                ?: event.changes.firstOrNull { it.pressed }
                                ?: break

                            if (!change.pressed) {
                                break
                            }

                            val deltaX = change.positionChange().x
                            if (deltaX != 0f) {
                                change.consume()
                                targetOffsetX = boundedSwipeOffset(targetOffsetX + deltaX)
                                if (!thresholdHapticSent && abs(targetOffsetX) >= actionThresholdPx) {
                                    thresholdHapticSent = true
                                    hapticFeedback.performHapticFeedback(HapticFeedbackType.LongPress)
                                }
                            }
                        }

                        isDragging = false
                        val finalOffset = targetOffsetX
                        when {
                            finalOffset <= -actionThresholdPx -> {
                                settleSpec = tween(120)
                                targetOffsetX = -maxRevealPx
                                scope.launch {
                                    kotlinx.coroutines.delay(120)
                                    onSwipeLeft()
                                    settleSpec = spring(stiffness = Spring.StiffnessHigh)
                                    targetOffsetX = 0f
                                }
                            }
                            finalOffset >= actionThresholdPx -> {
                                settleSpec = tween(120)
                                targetOffsetX = maxRevealPx
                                scope.launch {
                                    kotlinx.coroutines.delay(120)
                                    onSwipeRight()
                                    settleSpec = spring(stiffness = Spring.StiffnessHigh)
                                    targetOffsetX = 0f
                                }
                            }
                            else -> {
                                settleSpec = spring(stiffness = Spring.StiffnessMediumLow)
                                targetOffsetX = 0f
                            }
                        }
                    }
                }
        ) {
            content()
        }
    }
}

@Composable
private fun SwipeActionHint(
    action: AppSwipeAction,
    contentColor: Color,
    modifier: Modifier = Modifier
) {
    val isEndAction = action == AppSwipeAction.UmountModules
    Row(
        modifier = modifier,
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = if (isEndAction) Arrangement.End else Arrangement.Start
    ) {
        if (action == AppSwipeAction.UmountModules) {
            SwipeActionHintContent(
                icon = Icons.Filled.VisibilityOff,
                label = stringResource(R.string.profile_umount_modules),
                contentColor = contentColor
            )
        } else {
            SwipeActionHintContent(
                icon = Icons.Filled.Security,
                label = stringResource(R.string.category_root_apps),
                contentColor = contentColor
            )
        }
    }
}

@Composable
private fun SwipeActionHintContent(
    icon: ImageVector,
    label: String,
    contentColor: Color
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Icon(
            imageVector = icon,
            contentDescription = label,
            tint = contentColor
        )
        Text(
            text = label,
            color = contentColor,
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Bold
        )
    }
}

@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
private fun AppGroupItem(
    appGroup: SuperUserViewModel.AppGroup,
    isSelected: Boolean,
    onToggleSelection: () -> Unit,
    onClick: () -> Unit,
    onLongClick: () -> Unit,
    viewModel: SuperUserViewModel,
    expandedGroups: MutableState<Set<Int>>
) {
    val mainApp = appGroup.mainApp

    SwipeActionContainer(
        enabled = !viewModel.showBatchActions,
        onSwipeLeft = {
            viewModel.updateGroupPermission(
                appGroup = appGroup,
                allowSu = false,
                umountModules = !Natives.uidShouldUmount(appGroup.uid)
            )
        },
        onSwipeRight = {
            viewModel.updateGroupPermission(
                appGroup = appGroup,
                allowSu = !appGroup.allowSu
            )
        }
    ) {
        ListItem(
            modifier = Modifier.pointerInput(Unit) {
                detectTapGestures(
                    onLongPress = { onLongClick() },
                    onTap = { onClick() }
                )
            },
            headlineContent = {
                Text(mainApp.label)
            },
            supportingContent = {
                Column {
                    val summaryText = if (appGroup.apps.size > 1) {
                        stringResource(R.string.group_contains_apps, appGroup.apps.size)
                    } else {
                        mainApp.packageName
                    }

                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Text(summaryText)

                        if (appGroup.apps.size > 1) {
                            Icon(
                                imageVector = Icons.Default.KeyboardArrowDown,
                                contentDescription = null,
                                modifier = Modifier.rotate(
                                    animateFloatAsState(
                                        targetValue = if (expandedGroups.value.contains(appGroup.uid)) 180f else 0f,
                                        animationSpec = tween(200, easing = LinearOutSlowInEasing),
                                        label = ""
                                    ).value
                                )
                            )
                        }
                    }

                    FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        if (appGroup.allowSu) {
                            LabelItem(text = "ROOT")
                        } else {
                            if (Natives.uidShouldUmount(appGroup.uid)) {
                                LabelItem(
                                    text = "UMOUNT",
                                    style = LabelItemDefaults.style.copy(
                                        containerColor = MaterialTheme.colorScheme.secondaryContainer,
                                        contentColor = MaterialTheme.colorScheme.onSecondaryContainer
                                    )
                                )
                            }
                        }
                        if (appGroup.hasCustomProfile) {
                            LabelItem(
                                text = "CUSTOM",
                                style = LabelItemDefaults.style.copy(
                                    containerColor = MaterialTheme.colorScheme.tertiaryContainer,
                                    contentColor = MaterialTheme.colorScheme.onTertiaryContainer,
                                )
                            )
                        } else if (!appGroup.allowSu) {
                            LabelItem(
                                text = "DEFAULT",
                                style = LabelItemDefaults.style.copy(
                                    containerColor = Color.Gray
                                )
                            )
                        }
                        if (appGroup.apps.size > 1) {
                            appGroup.userName?.let {
                                LabelItem(
                                    text = it,
                                    style = LabelItemDefaults.style.copy(
                                        containerColor = MaterialTheme.colorScheme.primaryContainer,
                                        contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                                    )
                                )
                            }
                        }
                    }
                }
            },
            leadingContent = {
                AsyncImage(
                    model = ImageRequest.Builder(LocalContext.current)
                        .data(mainApp.packageInfo)
                        .crossfade(true)
                        .build(),
                    contentDescription = mainApp.label,
                    modifier = Modifier.padding(4.dp).width(48.dp).height(48.dp)
                )
            },
            trailingContent = {
                AnimatedVisibility(
                    visible = viewModel.showBatchActions,
                    enter = fadeIn(animationSpec = tween(200)) + scaleIn(
                        animationSpec = tween(200),
                        initialScale = 0.6f
                    ),
                    exit = fadeOut(animationSpec = tween(200)) + scaleOut(
                        animationSpec = tween(200),
                        targetScale = 0.6f
                    )
                ) {
                    val checkboxInteractionSource = remember { MutableInteractionSource() }
                    val isCheckboxPressed by checkboxInteractionSource.collectIsPressedAsState()

                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.End
                    ) {
                        AnimatedVisibility(
                            visible = isCheckboxPressed,
                            enter = expandHorizontally() + fadeIn(),
                            exit = shrinkHorizontally() + fadeOut()
                        ) {
                            Text(
                                text = if (isSelected) stringResource(R.string.selected) else stringResource(R.string.select),
                                style = MaterialTheme.typography.labelMedium,
                                modifier = Modifier.padding(end = 4.dp)
                            )
                        }
                        Checkbox(
                            checked = isSelected,
                            onCheckedChange = { onToggleSelection() },
                            interactionSource = checkboxInteractionSource,
                        )
                    }
                }
            }
        )
    }
}
