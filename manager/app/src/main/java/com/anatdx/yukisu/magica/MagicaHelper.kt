package com.anatdx.yukisu.magica

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.util.Log
import androidx.core.content.edit

object MagicaHelper {
    private const val TAG = "YukiSUMagica"
    private const val SETTINGS_PREFS = "settings"
    private const val AUTO_JAILBREAK_KEY = "auto_jailbreak"

    @JvmStatic
    fun isAutoJailbreakEnabled(context: Context): Boolean {
        return context.applicationContext
            .getSharedPreferences(SETTINGS_PREFS, Context.MODE_PRIVATE)
            .getBoolean(AUTO_JAILBREAK_KEY, false)
    }

    @JvmStatic
    fun setAutoJailbreakEnabled(context: Context, enabled: Boolean) {
        val appContext = context.applicationContext
        runCatching {
            appContext.packageManager.setComponentEnabledSetting(
                ComponentName(appContext, BootCompletedReceiver::class.java),
                if (enabled) {
                    PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                } else {
                    PackageManager.COMPONENT_ENABLED_STATE_DISABLED
                },
                PackageManager.DONT_KILL_APP
            )
        }.onFailure {
            Log.e(TAG, "failed to update auto jailbreak receiver state to $enabled", it)
        }

        appContext.getSharedPreferences(SETTINGS_PREFS, Context.MODE_PRIVATE).edit {
            putBoolean(AUTO_JAILBREAK_KEY, enabled)
        }
    }

    @JvmStatic
    fun launch(context: Context, trigger: String = "manual"): Boolean {
        val appContext = context.applicationContext
        return runCatching {
            appContext.startService(Intent(appContext, MagicaService::class.java))
            Log.i(TAG, "MagicaService started from trigger: $trigger")
            true
        }.getOrElse {
            Log.e(TAG, "failed to start MagicaService from trigger: $trigger", it)
            false
        }
    }
}
