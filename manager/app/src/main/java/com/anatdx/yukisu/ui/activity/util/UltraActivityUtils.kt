package com.anatdx.yukisu.ui.activity.util

import android.content.Context
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.runtime.Composable
import androidx.lifecycle.LifecycleCoroutineScope
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.ui.MainActivity
import com.anatdx.yukisu.ui.util.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.*
import android.net.Uri
import androidx.lifecycle.lifecycleScope
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.ui.component.ZipFileDetector
import com.anatdx.yukisu.ui.component.ZipFileInfo
import com.anatdx.yukisu.ui.component.ZipType
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.anatdx.yukisu.ui.screen.FlashIt
import kotlinx.coroutines.withContext
import androidx.core.content.edit

object AnimatedBottomBar {
    @Composable
    fun AnimatedBottomBarWrapper(
        showBottomBar: Boolean,
        content: @Composable () -> Unit
    ) {
        AnimatedVisibility(
            visible = showBottomBar,
            enter = slideInVertically(initialOffsetY = { it }) + fadeIn(),
            exit = slideOutVertically(targetOffsetY = { it }) + fadeOut()
        ) {
            content()
        }
    }
}

object UltraActivityUtils {

    suspend fun detectZipTypeAndShowConfirmation(
        activity: MainActivity,
        zipUris: ArrayList<Uri>,
        onResult: (List<ZipFileInfo>) -> Unit
    ) {
        val infos = ZipFileDetector.detectAndParseZipFiles(activity, zipUris)
        withContext(Dispatchers.Main) { onResult(infos) }
    }

    fun navigateToFlashScreen(
        activity: MainActivity,
        zipFiles: List<ZipFileInfo>,
        navigator: DestinationsNavigator
    ) {
        activity.lifecycleScope.launch {
            val moduleUris = zipFiles.filter { it.type == ZipType.MODULE }.map { it.uri }

            if (moduleUris.isNotEmpty()) {
                navigator.navigate(
                    FlashScreenDestination(
                        FlashIt.FlashModules(ArrayList(moduleUris))
                    )
                )
            }
        }
    }
}

object AppData {
    object DataRefreshManager {
        private val _superuserCount = MutableStateFlow(0)
        private val _moduleCount = MutableStateFlow(0)
        private val _isFullFeatured = MutableStateFlow(false)

        val superuserCount: StateFlow<Int> = _superuserCount.asStateFlow()
        val moduleCount: StateFlow<Int> = _moduleCount.asStateFlow()
        val isFullFeatured: StateFlow<Boolean> = _isFullFeatured.asStateFlow()

        fun refreshData() {
            val sc = getSuperuserCountUse()
            val mc = getModuleCountUse()
            val ff = isFullFeatured()
            if (_superuserCount.value != sc) _superuserCount.value = sc
            if (_moduleCount.value != mc) _moduleCount.value = mc
            if (_isFullFeatured.value != ff) _isFullFeatured.value = ff
        }
    }

    fun getSuperuserCountUse(): Int {
        return try {
            if (!rootAvailable()) return 0
            getSuperuserCount()
        } catch (_: Exception) {
            0
        }
    }

    fun getModuleCountUse(): Int {
        return try {
            if (!rootAvailable()) return 0
            getModuleCount()
        } catch (_: Exception) {
            0
        }
    }

    fun isFullFeatured(): Boolean {
        val isManager = Natives.isManager
        return isManager && !Natives.requireNewKernel() && rootAvailable()
    }
}

object DataRefreshUtils {
    fun startDataRefreshCoroutine(scope: LifecycleCoroutineScope) {
        scope.launch(Dispatchers.IO) {
            while (isActive) {
                AppData.DataRefreshManager.refreshData()
                delay(10000) // 10s，减少轮询频率
            }
        }
    }

    fun startSettingsMonitorCoroutine(
        scope: LifecycleCoroutineScope,
        activity: MainActivity,
        settingsStateFlow: MutableStateFlow<MainActivity.SettingsState>
    ) {
        scope.launch(Dispatchers.IO) {
            while (isActive) {
                val prefs = activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
                val newState = MainActivity.SettingsState(
                    isHideOtherInfo = prefs.getBoolean("is_hide_other_info", false)
                )
                if (settingsStateFlow.value != newState) {
                    settingsStateFlow.value = newState
                }
                delay(5000) // 5s，减少轮询频率
            }
        }
    }

    fun refreshData(scope: LifecycleCoroutineScope) {
        scope.launch {
            AppData.DataRefreshManager.refreshData()
        }
    }
}

object DisplayUtils {
    fun applyCustomDpi(context: Context) {
        val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        val customDpi = prefs.getInt("app_dpi", 0)

        if (customDpi > 0) {
            try {
                val resources = context.resources
                val metrics = resources.displayMetrics
                metrics.density = customDpi / 160f
                @Suppress("DEPRECATION")
                metrics.scaledDensity = customDpi / 160f
                metrics.densityDpi = customDpi
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }
    }
}
