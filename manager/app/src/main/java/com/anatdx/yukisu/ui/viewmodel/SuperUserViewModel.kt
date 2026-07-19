package com.anatdx.yukisu.ui.viewmodel

import android.content.*
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.graphics.drawable.Drawable
import android.os.IBinder
import android.os.Parcelable
import android.util.Log
import androidx.compose.runtime.*
import androidx.core.content.edit
import androidx.lifecycle.ViewModel
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.ksuApp
import com.anatdx.yukisu.ui.KsuService
import com.anatdx.yukisu.ui.util.*
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.text.Collator
import java.util.*
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine
import com.anatdx.yukisu.IKsuInterface
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.parcelize.IgnoredOnParcel
import kotlinx.parcelize.Parcelize

enum class AppCategory(val displayNameRes: Int, val persistKey: String) {
    ALL(com.anatdx.yukisu.R.string.category_all_apps, "ALL"),
    ROOT(com.anatdx.yukisu.R.string.category_root_apps, "ROOT"),
    CUSTOM(com.anatdx.yukisu.R.string.category_custom_apps, "CUSTOM"),
    DEFAULT(com.anatdx.yukisu.R.string.category_default_apps, "DEFAULT");

    companion object {
        fun fromPersistKey(key: String): AppCategory = entries.find { it.persistKey == key } ?: ALL
    }
}

enum class SortType(val displayNameRes: Int, val persistKey: String) {
    NAME_ASC(com.anatdx.yukisu.R.string.sort_name_asc, "NAME_ASC"),
    NAME_DESC(com.anatdx.yukisu.R.string.sort_name_desc, "NAME_DESC"),
    INSTALL_TIME_NEW(com.anatdx.yukisu.R.string.sort_install_time_new, "INSTALL_TIME_NEW"),
    INSTALL_TIME_OLD(com.anatdx.yukisu.R.string.sort_install_time_old, "INSTALL_TIME_OLD"),
    SIZE_DESC(com.anatdx.yukisu.R.string.sort_size_desc, "SIZE_DESC"),
    SIZE_ASC(com.anatdx.yukisu.R.string.sort_size_asc, "SIZE_ASC"),
    USAGE_FREQ(com.anatdx.yukisu.R.string.sort_usage_freq, "USAGE_FREQ");

    companion object {
        fun fromPersistKey(key: String): SortType = entries.find { it.persistKey == key } ?: NAME_ASC
    }
}

class SuperUserViewModel : ViewModel() {
    companion object {
        private const val TAG = "SuperUserViewModel"
        const val SHELL_UID = 2000
        private val appsLock = Any()
        var apps by mutableStateOf<List<AppInfo>>(emptyList())
        private val _isAppListLoaded = MutableStateFlow(false)
        val isAppListLoaded = _isAppListLoaded.asStateFlow()

        @JvmStatic
        fun getAppIconDrawable(context: Context, packageName: String): Drawable? {
            val appList = synchronized(appsLock) { apps }
            return appList.find { it.packageName == packageName }
                ?.packageInfo?.applicationInfo?.loadIcon(context.packageManager)
        }

        var appGroups by mutableStateOf<List<AppGroup>>(emptyList())

        private const val PREFS_NAME = "settings"
        private const val KEY_SHOW_SYSTEM_APPS = "show_system_apps"
        private const val KEY_SELECTED_CATEGORY = "selected_category"
        private const val KEY_CURRENT_SORT_TYPE = "current_sort_type"
        private const val CORE_POOL_SIZE = 8
        private const val MAX_POOL_SIZE = 16
        private const val KEEP_ALIVE_TIME = 60L
        private const val BATCH_SIZE = 20
        private const val PER_USER_RANGE = 100000
    }

    @Immutable
    @Parcelize
    data class AppInfo(
        val label: String,
        val packageInfo: PackageInfo,
        val profile: Natives.Profile?,
    ) : Parcelable {
        @IgnoredOnParcel
        val packageName: String = packageInfo.packageName
        @IgnoredOnParcel
        val uid: Int = packageInfo.applicationInfo!!.uid
        @IgnoredOnParcel
        val profileKey: String = profile?.name?.takeIf { it.isNotBlank() } ?: packageName
    }

    @Immutable
    @Parcelize
    data class AppGroup(
        val uid: Int,
        val apps: List<AppInfo>,
        val profile: Natives.Profile?,
        val dynamicManagerFlags: Int = 0
    ) : Parcelable {
        @IgnoredOnParcel
        val mainApp: AppInfo = apps.first()
        @IgnoredOnParcel
        val packageNames: List<String> = apps.map { it.packageName }
        @IgnoredOnParcel
        val profileKey: String = mainApp.profileKey
        @IgnoredOnParcel
        val allowSu: Boolean = profile?.allowSu == true
        @IgnoredOnParcel
        val isPresetManager: Boolean =
            dynamicManagerFlags and Natives.DYNAMIC_MANAGER_FLAG_PRESET != 0
        @IgnoredOnParcel
        val isDynamicManager: Boolean =
            dynamicManagerFlags and Natives.DYNAMIC_MANAGER_FLAG_TRUSTED != 0
        @IgnoredOnParcel
        val userName: String? = Natives.getUserName(uid)
        @IgnoredOnParcel
        val hasCustomProfile : Boolean = profile?.let { if (it.allowSu) !it.rootUseDefault else !it.nonRootUseDefault } ?: false
    }

    private val appProcessingThreadPool = ThreadPoolExecutor(
        CORE_POOL_SIZE, MAX_POOL_SIZE, KEEP_ALIVE_TIME, TimeUnit.SECONDS,
        LinkedBlockingQueue()
    ) { runnable ->
        Thread(runnable, "AppProcessing-${System.currentTimeMillis()}").apply {
            isDaemon = true
            priority = Thread.NORM_PRIORITY
        }
    }.asCoroutineDispatcher()

    private val appListMutex = Mutex()
    private val configChangeListeners = mutableSetOf<(String) -> Unit>()
    private val prefs = ksuApp.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    var search by mutableStateOf("")
    var showSystemApps by mutableStateOf(prefs.getBoolean(KEY_SHOW_SYSTEM_APPS, false))
        private set
    var selectedCategory by mutableStateOf(loadSelectedCategory())
        private set
    var currentSortType by mutableStateOf(loadCurrentSortType())
        private set
    var isRefreshing by mutableStateOf(false)
        private set
    var showBatchActions by mutableStateOf(false)
        internal set
    var selectedApps by mutableStateOf<Set<String>>(emptySet())
        internal set
    var loadingProgress by mutableFloatStateOf(0f)
        private set

    private fun loadSelectedCategory(): AppCategory {
        val categoryKey = prefs.getString(KEY_SELECTED_CATEGORY, AppCategory.ALL.persistKey)
            ?: AppCategory.ALL.persistKey
        return AppCategory.fromPersistKey(categoryKey)
    }

    private fun loadCurrentSortType(): SortType {
        val sortKey = prefs.getString(KEY_CURRENT_SORT_TYPE, SortType.NAME_ASC.persistKey)
            ?: SortType.NAME_ASC.persistKey
        return SortType.fromPersistKey(sortKey)
    }

    fun updateShowSystemApps(newValue: Boolean) {
        showSystemApps = newValue
        prefs.edit { putBoolean(KEY_SHOW_SYSTEM_APPS, newValue) }
        notifyAppListChanged()
    }

    private fun notifyAppListChanged() {
        val currentApps = apps
        apps = emptyList()
        apps = currentApps
    }

    fun updateSelectedCategory(newCategory: AppCategory) {
        selectedCategory = newCategory
        prefs.edit { putString(KEY_SELECTED_CATEGORY, newCategory.persistKey) }
    }

    fun updateCurrentSortType(newSortType: SortType) {
        currentSortType = newSortType
        prefs.edit { putString(KEY_CURRENT_SORT_TYPE, newSortType.persistKey) }
    }

    fun toggleBatchMode() {
        showBatchActions = !showBatchActions
        if (!showBatchActions) clearSelection()
    }

    fun toggleAppSelection(packageName: String) {
        selectedApps = if (selectedApps.contains(packageName)) {
            selectedApps - packageName
        } else {
            selectedApps + packageName
        }
    }

    fun clearSelection() {
        selectedApps = emptySet()
    }

    suspend fun updateBatchPermissions(allowSu: Boolean, umountModules: Boolean? = null) {
        val selectedUids = apps.asSequence()
            .filter { it.packageName in selectedApps }
            .map { it.uid }
            .toSet()

        selectedUids.forEach { uid ->
            appGroups.find { it.uid == uid }?.let { group ->
                val profile = Natives.getAppProfile(group.profileKey, uid)
                val updatedProfile = profile.copy(
                    allowSu = allowSu,
                    umountModules = umountModules ?: profile.umountModules,
                    nonRootUseDefault = false
                )
                if (Natives.setAppProfile(updatedProfile)) {
                    updateUidProfileLocally(uid, updatedProfile)
                    notifyConfigChange(group.profileKey)
                }
            }
        }
        clearSelection()
        showBatchActions = false
        refreshAppConfigurations()
    }

    suspend fun updateGroupPermission(
        appGroup: AppGroup,
        allowSu: Boolean,
        umountModules: Boolean? = null
    ): Boolean {
        val profile = Natives.getAppProfile(appGroup.profileKey, appGroup.uid)
        val updatedProfile = profile.copy(
            allowSu = allowSu,
            umountModules = umountModules ?: profile.umountModules,
            nonRootUseDefault = if (!allowSu && umountModules != null) {
                !umountModules
            } else {
                profile.nonRootUseDefault
            }
        )
        val updated = Natives.setAppProfile(updatedProfile)

        if (updated) {
            updateUidProfileLocally(appGroup.uid, updatedProfile)
            notifyConfigChange(appGroup.profileKey)
            refreshAppConfigurations()
        }

        return updated
    }

    fun updateUidProfileLocally(uid: Int, updatedProfile: Natives.Profile) {
        appListMutex.tryLock().let { locked ->
            if (locked) {
                try {
                    apps = apps.map { app ->
                        if (app.uid == uid) {
                            app.copy(profile = updatedProfile)
                        } else app
                    }
                    appGroups = groupAppsByUid(apps)
                } finally {
                    appListMutex.unlock()
                }
            }
        }
    }

    private fun notifyConfigChange(packageName: String) {
        configChangeListeners.forEach { listener ->
            try {
                listener(packageName)
            } catch (e: Exception) {
                Log.e(TAG, "Error notifying config change for $packageName", e)
            }
        }
    }

    suspend fun refreshAppConfigurations() {
        withContext(appProcessingThreadPool) {
            supervisorScope {
                val currentApps = apps.toList()
                val uidGroups = currentApps.groupBy { it.uid }.values.toList()
                val batches = uidGroups.chunked(BATCH_SIZE)
                loadingProgress = 0f

                val updatedApps = batches.mapIndexed { batchIndex, batch ->
                    async {
                        val batchResult = batch.flatMap { uidApps ->
                            try {
                                val profile = loadUidProfile(uidApps)
                                uidApps.map { it.copy(profile = profile) }
                            } catch (e: Exception) {
                                Log.e(TAG, "Error refreshing profile for uid ${uidApps.first().uid}", e)
                                uidApps
                            }
                        }
                        loadingProgress = (batchIndex + 1).toFloat() / batches.size.coerceAtLeast(1)
                        batchResult
                    }
                }.awaitAll().flatten()

                appListMutex.withLock {
                    apps = updatedApps
                    appGroups = groupAppsByUid(updatedApps)
                }
                loadingProgress = 1f
            }
        }
    }

    private var serviceConnection: ServiceConnection? = null

    private suspend fun connectKsuService(onDisconnect: () -> Unit = {}): IBinder? =
        suspendCoroutine { continuation ->
            val connection = object : ServiceConnection {
                override fun onServiceDisconnected(name: ComponentName?) {
                    onDisconnect()
                    serviceConnection = null
                }
                override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
                    continuation.resume(binder)
                }
            }
            serviceConnection = connection
            val intent = Intent(ksuApp, KsuService::class.java)
            try {
                val task = com.topjohnwu.superuser.ipc.RootService.bindOrTask(
                    intent, Shell.EXECUTOR, connection
                )
                task?.let { Shell.getShell().execTask(it) }
            } catch (e: Exception) {
                Log.e(TAG, "Failed to bind KsuService", e)
                continuation.resume(null)
            }
        }

    private fun stopKsuService() {
        serviceConnection?.let {
            try {
                val intent = Intent(ksuApp, KsuService::class.java)
                com.topjohnwu.superuser.ipc.RootService.stop(intent)
                serviceConnection = null
            } catch (e: Exception) {
                Log.e(TAG, "Failed to stop KsuService", e)
            }
        }
    }

    suspend fun fetchAppList() {
        isRefreshing = true
        loadingProgress = 0f

        val binder = connectKsuService() ?: run { isRefreshing = false; return }

        withContext(Dispatchers.IO) {
            val pm = ksuApp.packageManager
            val allPackages = IKsuInterface.Stub.asInterface(binder)
            val total = allPackages.packageCount
            val pageSize = 100
            val result = mutableListOf<AppInfo>()

            var start = 0
            while (start < total) {
                val page = allPackages.getPackages(start, pageSize)
                if (page.isEmpty()) break

                result += page.mapNotNull { packageInfo ->
                    packageInfo.applicationInfo?.let { appInfo ->
                        AppInfo(
                            label = appInfo.loadLabel(pm).toString(),
                            packageInfo = packageInfo,
                            profile = null
                        )
                    }
                }
                start += page.size
                loadingProgress = start.toFloat() / total
            }

            stopKsuService()

            synchronized(appsLock) {
                _isAppListLoaded.value = true
            }

            appListMutex.withLock {
                val filteredApps = result.filter { it.packageName != ksuApp.packageName }
                val profiledApps = filteredApps.groupBy { it.uid }.values.flatMap { uidApps ->
                    val profile = loadUidProfile(uidApps)
                    uidApps.map { it.copy(profile = profile) }
                }
                apps = profiledApps
                appGroups = groupAppsByUid(profiledApps)
            }
            loadingProgress = 1f
        }
        isRefreshing = false
    }

    val appGroupList by derivedStateOf {
        appGroups.filter { group ->
            group.apps.any { app ->
                app.label.contains(search, true) ||
                        app.packageName.contains(search, true) ||
                        HanziToPinyin.getInstance().toPinyinString(app.label)?.contains(search, true) == true
            }
        }.filter { group ->
            group.uid == SHELL_UID || showSystemApps ||
                    group.apps.any { it.packageInfo.applicationInfo!!.flags.and(ApplicationInfo.FLAG_SYSTEM) == 0 }
        }
    }

    private fun loadDynamicManagerFlags(): Map<Int, Int> {
        val items = runCatching { Natives.getDynamicManagers() }.getOrDefault(IntArray(0))
        if (items.size < 2) return emptyMap()

        val result = mutableMapOf<Int, Int>()
        var index = 0
        while (index + 1 < items.size) {
            val appId = items[index]
            val flags = items[index + 1]
            result[appId] = flags
            index += 2
        }
        return result
    }

    private fun groupAppsByUid(appList: List<AppInfo>): List<AppGroup> {
        val dynamicManagers = loadDynamicManagerFlags()

        return appList.groupBy { it.uid }
            .map { (uid, apps) ->
                val sortedApps = apps.sortedBy { it.label }
                val profile = apps.firstOrNull()?.profile
                val dynamicManagerFlags = dynamicManagers[uid % PER_USER_RANGE] ?: 0
                AppGroup(
                    uid = uid,
                    apps = sortedApps,
                    profile = profile,
                    dynamicManagerFlags = dynamicManagerFlags
                )
            }
            .sortedWith(
                compareBy<AppGroup> {
                    when {
                        it.isDynamicManager -> 0
                        it.uid == SHELL_UID -> 1
                        it.allowSu -> 2
                        it.hasCustomProfile -> 3
                        else -> 4
                    }
                }.thenBy(Collator.getInstance(Locale.getDefault())) {
                    it.userName?.takeIf { name -> name.isNotBlank() } ?: it.uid.toString()
                }.thenBy(Collator.getInstance(Locale.getDefault())) { it.mainApp.label }
            )
    }

    private fun loadUidProfile(uidApps: Collection<AppInfo>): Natives.Profile {
        val first = uidApps.first()
        val packageNames = uidApps.mapTo(mutableSetOf()) { it.packageName }
        val fallbackKey = packageNames.min()
        val profile = Natives.getAppProfile(fallbackKey, first.uid)
        return if (profile.name in packageNames) profile else profile.copy(name = fallbackKey)
    }
    override fun onCleared() {
        try {
            stopKsuService()
            appProcessingThreadPool.close()
            configChangeListeners.clear()
        } catch (e: Exception) {
            Log.e(TAG, "Error cleaning up resources", e)
        }
    }
}
