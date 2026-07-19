package com.anatdx.yukisu

import android.app.Application
import android.os.Build
import android.os.Process
import android.system.Os
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import androidx.lifecycle.ViewModelStoreOwner
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import com.anatdx.yukisu.ui.viewmodel.SuperUserViewModel
import coil.Coil
import coil.ImageLoader
import com.dergoogler.mmrl.platform.Platform
import me.zhanghai.android.appiconloader.coil.AppIconFetcher
import me.zhanghai.android.appiconloader.coil.AppIconKeyer
import okhttp3.Cache
import okhttp3.OkHttpClient
import java.io.File
import java.util.Locale

lateinit var ksuApp: KernelSUApplication

class KernelSUApplication : Application(), ViewModelStoreOwner {

    lateinit var okhttpClient: OkHttpClient
    private val appViewModelStore by lazy { ViewModelStore() }

    override fun onCreate() {
        super.onCreate()
        ksuApp = this

        // Isolated Magica processes only need TMPDIR for the bundled ksud bootstrap.
        if (isIsolatedProcess()) {
            Os.setenv("TMPDIR", cacheDir.absolutePath, true)
            return
        }

        // For faster response when first entering superuser or webui activity
        val superUserViewModel = ViewModelProvider(this)[SuperUserViewModel::class.java]
        CoroutineScope(Dispatchers.Main).launch {
            superUserViewModel.fetchAppList()
        }

        Platform.setHiddenApiExemptions()

        val context = this
        val iconSize = resources.getDimensionPixelSize(android.R.dimen.app_icon_size)
        Coil.setImageLoader(
            ImageLoader.Builder(context)
                .components {
                    add(AppIconKeyer())
                    add(AppIconFetcher.Factory(iconSize, false, context))
                }
                .build()
        )

        val webroot = File(dataDir, "webroot")
        if (!webroot.exists()) {
            webroot.mkdir()
        }

        // Provide working env for rust's temp_dir()
        Os.setenv("TMPDIR", cacheDir.absolutePath, true)

        okhttpClient =
            OkHttpClient.Builder().cache(Cache(File(cacheDir, "okhttp"), 10 * 1024 * 1024))
                .addInterceptor { block ->
                    block.proceed(
                        block.request().newBuilder()
                            .header("User-Agent", "YukiSU/${BuildConfig.VERSION_CODE}")
                            .header("Accept-Language", Locale.getDefault().toLanguageTag()).build()
                    )
                }.build()
    }
    override val viewModelStore: ViewModelStore
        get() = appViewModelStore

    private fun isIsolatedProcess(): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            return Process.isIsolated()
        }
        // FIRST_ISOLATED_UID..LAST_ISOLATED_UID on Android 8.x.
        val appId = Process.myUid() % 100_000
        return appId in 99_000..99_999
    }
}
