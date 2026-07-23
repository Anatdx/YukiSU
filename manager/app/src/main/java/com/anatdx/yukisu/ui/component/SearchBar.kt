package com.anatdx.yukisu.ui.component

import android.util.Log
import androidx.compose.animation.*
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.core.updateTransition
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import kotlinx.coroutines.flow.first

private const val TAG = "SearchBar"

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SearchAppBar(
    title: @Composable () -> Unit,
    searchText: String,
    onSearchTextChange: (String) -> Unit,
    onClearClick: () -> Unit,
    placeholder: @Composable (() -> Unit)? = null,
    onBackClick: (() -> Unit)? = null,
    onConfirm: (() -> Unit)? = null,
    dropdownContent: @Composable (() -> Unit)? = null,
    scrollBehavior: TopAppBarScrollBehavior? = null
) {
    val keyboardController = LocalSoftwareKeyboardController.current
    val focusRequester = remember { FocusRequester() }
    var onSearch by remember { mutableStateOf(false) }
    val expressiveUi = isExpressiveUi
    var compactHeader by remember { mutableStateOf(false) }
    var searchFieldVisible by remember { mutableStateOf(false) }
    val headerTransition = updateTransition(
        targetState = compactHeader,
        label = "ExpressiveSearchHeader",
    )
    val searchTransition = updateTransition(
        targetState = searchFieldVisible,
        label = "ExpressiveSearchField",
    )

    val colorScheme = MaterialTheme.colorScheme
    val cardColor = if (CardConfig.isCustomBackgroundEnabled) {
        colorScheme.surfaceContainerLow
    } else {
        colorScheme.background
    }
    LaunchedEffect(onSearch, expressiveUi) {
        if (expressiveUi) {
            if (onSearch) {
                compactHeader = true
                snapshotFlow {
                    headerTransition.currentState && !headerTransition.isRunning
                }.first { it }
                searchFieldVisible = true
            } else {
                searchFieldVisible = false
                // Start restoring the large header just before the deterministic 180 ms
                // field exit finishes, avoiding a one-to-two-frame hand-off pause.
                delay(150)
                compactHeader = false
            }
        }
    }
    val visibleSearch = if (expressiveUi) searchFieldVisible else onSearch
    LaunchedEffect(visibleSearch) {
        if (visibleSearch) {
            if (expressiveUi) delay(50)
            focusRequester.requestFocus()
        }
    }
    DisposableEffect(Unit) {
        onDispose {
            keyboardController?.hide()
        }
    }

    val expressiveSearchEnter = expandHorizontally(
        animationSpec = MaterialTheme.motionScheme.defaultSpatialSpec<IntSize>(),
        expandFrom = Alignment.End,
        clip = true,
        initialWidth = { fullWidth -> fullWidth / 10 },
    )
    val expressiveSearchExit = shrinkHorizontally(
        animationSpec = tween(
            durationMillis = 180,
            easing = FastOutSlowInEasing,
        ),
        shrinkTowards = Alignment.End,
        clip = true,
        targetWidth = { 0 },
    )

    val searchField: @Composable () -> Unit = {
        val horizontalOffset = if (expressiveUi) 8.dp else 0.dp
        val endPadding = when {
            onBackClick != null -> 0.dp
            expressiveUi -> 6.dp
            else -> 14.dp
        }
        OutlinedTextField(
            shape = if (expressiveUi) {
                CircleShape
            } else {
                OutlinedTextFieldDefaults.shape
            },
            modifier = Modifier
                .fillMaxWidth()
                .padding(
                    top = 2.dp,
                    bottom = 2.dp,
                    start = horizontalOffset,
                    end = endPadding,
                )
                .focusRequester(focusRequester)
                .onFocusChanged { focusState ->
                    if (focusState.isFocused) onSearch = true
                    Log.d(TAG, "onFocusChanged: $focusState")
                },
            value = searchText,
            onValueChange = onSearchTextChange,
            placeholder = placeholder,
            trailingIcon = {
                IconButton(
                    onClick = {
                        onSearch = false
                        keyboardController?.hide()
                        onClearClick()
                    },
                    content = { YukiIcon(Icons.Filled.Close, null) }
                )
            },
            maxLines = 1,
            singleLine = true,
            keyboardOptions = KeyboardOptions.Default.copy(
                imeAction = ImeAction.Done
            ),
            keyboardActions = KeyboardActions(onDone = {
                keyboardController?.hide()
                onConfirm?.invoke()
            })
        )
    }

    val compactTopBar: @Composable (Boolean) -> Unit = { searching ->
        val showCompactTitle = if (expressiveUi) {
            !searchTransition.currentState && !searchTransition.targetState
        } else {
            !searching
        }
        TopAppBar(
            title = {
                Box {
                    AnimatedVisibility(
                        modifier = Modifier.align(Alignment.CenterStart),
                        visible = showCompactTitle,
                        enter = fadeIn(),
                        exit = fadeOut(),
                        content = { title() }
                    )

                    if (expressiveUi) {
                        searchTransition.AnimatedVisibility(
                            visible = { it },
                            enter = expressiveSearchEnter,
                            exit = expressiveSearchExit,
                            content = { searchField() },
                        )
                    } else {
                        AnimatedVisibility(
                            visible = searching,
                            enter = fadeIn(),
                            exit = fadeOut(),
                            content = { searchField() },
                        )
                    }
                }
            },
            navigationIcon = {
                if (onBackClick != null) {
                    IconButton(
                        onClick = onBackClick,
                        content = { YukiIcon(Icons.AutoMirrored.Outlined.ArrowBack, null) }
                    )
                }
            },
            actions = {
                AnimatedVisibility(visible = showCompactTitle) {
                    IconButton(
                        onClick = { onSearch = true },
                        content = { YukiIcon(Icons.Filled.Search, null) }
                    )
                }
                dropdownContent?.invoke()
            },
            windowInsets = WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal
            ),
            // Search replaces the flexible header with a compact field. Do not carry the
            // collapsed header offset into that field, otherwise it can start off-screen.
            scrollBehavior = if (searching || expressiveUi) null else scrollBehavior,
            colors = TopAppBarDefaults.topAppBarColors(
                containerColor = cardColor,
                scrolledContainerColor = cardColor
            )
        )
    }

    if (isExpressiveUi) {
        val headerEffectsSpec = tween<Float>(durationMillis = 90)
        val headerSizeSpec = tween<IntSize>(
            durationMillis = 110,
            easing = FastOutSlowInEasing,
        )

        headerTransition.AnimatedContent(
            contentAlignment = Alignment.TopCenter,
            transitionSpec = {
                fadeIn(animationSpec = headerEffectsSpec).togetherWith(
                    fadeOut(animationSpec = headerEffectsSpec)
                ).using(
                    SizeTransform(
                        clip = false,
                        sizeAnimationSpec = { _, _ -> headerSizeSpec },
                    )
                )
            },
        ) { compact ->
            if (compact) {
                compactTopBar(searchFieldVisible)
            } else {
                LargeFlexibleTopAppBar(
                    title = title,
                    navigationIcon = {
                        if (onBackClick != null) {
                            IconButton(
                                onClick = onBackClick,
                                content = {
                                    YukiIcon(Icons.AutoMirrored.Outlined.ArrowBack, null)
                                }
                            )
                        }
                    },
                    actions = {
                        IconButton(
                            onClick = { onSearch = true },
                            content = { YukiIcon(Icons.Filled.Search, null) }
                        )
                        dropdownContent?.invoke()
                    },
                    windowInsets = WindowInsets.safeDrawing.only(
                        WindowInsetsSides.Top + WindowInsetsSides.Horizontal
                    ),
                    scrollBehavior = scrollBehavior,
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = cardColor,
                        scrolledContainerColor = cardColor
                    )
                )
            }
        }
    } else {
        compactTopBar(onSearch)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Preview
@Composable
private fun SearchAppBarPreview() {
    var searchText by remember { mutableStateOf("") }
    SearchAppBar(
        title = { Text("Search text") },
        searchText = searchText,
        onSearchTextChange = { searchText = it },
        onClearClick = { searchText = "" }
    )
}
