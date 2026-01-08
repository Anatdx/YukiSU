package com.anatdx.yukisu.ui.viewmodel

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import io.murasaki.HymoFs
import io.murasaki.KsuService
import io.murasaki.Murasaki
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Murasaki 服务状态 ViewModel
 * 
 * 用于管理 Murasaki API 的连接状态和数据展示
 */
class MurasakiViewModel : ViewModel() {

    /**
     * Murasaki 服务状态
     */
    data class MurasakiStatus(
        val isConnected: Boolean = false,
        val serviceVersion: Int = -1,
        val ksuVersion: Int = -1,
        val privilegeLevel: Int = -1,
        val privilegeLevelName: String = "Unknown",
        val isKernelModeAvailable: Boolean = false,
        val selinuxContext: String? = null,
        val error: String? = null
    )

    /**
     * HymoFS 状态
     */
    data class HymoFsStatus(
        val activeRules: String? = null,
        val stealthEnabled: Boolean? = null,
        val debugEnabled: Boolean? = null
    )

    // 状态
    var murasakiStatus by mutableStateOf(MurasakiStatus())
        private set

    var hymoFsStatus by mutableStateOf(HymoFsStatus())
        private set

    var isLoading by mutableStateOf(false)
        private set

    /**
     * 初始化连接
     */
    fun connect() {
        viewModelScope.launch {
            isLoading = true
            try {
                val result = withContext(Dispatchers.IO) {
                    val level = Murasaki.init()
                    if (level < 0) {
                        return@withContext MurasakiStatus(
                            isConnected = false,
                            error = "Failed to connect to Murasaki service"
                        )
                    }

                    MurasakiStatus(
                        isConnected = true,
                        serviceVersion = Murasaki.getVersion(),
                        ksuVersion = Murasaki.getKsuVersion(),
                        privilegeLevel = level,
                        privilegeLevelName = getPrivilegeLevelName(level),
                        isKernelModeAvailable = Murasaki.isKernelModeAvailable(),
                        selinuxContext = Murasaki.getSelinuxContext()
                    )
                }
                murasakiStatus = result
            } catch (e: Exception) {
                murasakiStatus = MurasakiStatus(
                    isConnected = false,
                    error = e.message ?: "Unknown error"
                )
            } finally {
                isLoading = false
            }
        }
    }

    /**
     * 刷新 HymoFS 状态
     */
    fun refreshHymoFsStatus() {
        viewModelScope.launch {
            try {
                val rules = withContext(Dispatchers.IO) {
                    HymoFs.getActiveRules()
                }
                hymoFsStatus = hymoFsStatus.copy(activeRules = rules)
            } catch (e: Exception) {
                // Ignore
            }
        }
    }

    /**
     * 设置隐身模式
     */
    fun setStealthMode(enabled: Boolean) {
        viewModelScope.launch {
            try {
                val success = withContext(Dispatchers.IO) {
                    HymoFs.setStealth(enabled)
                }
                if (success) {
                    hymoFsStatus = hymoFsStatus.copy(stealthEnabled = enabled)
                }
            } catch (e: Exception) {
                // Ignore
            }
        }
    }

    /**
     * 设置调试模式
     */
    fun setDebugMode(enabled: Boolean) {
        viewModelScope.launch {
            try {
                val success = withContext(Dispatchers.IO) {
                    HymoFs.setDebug(enabled)
                }
                if (success) {
                    hymoFsStatus = hymoFsStatus.copy(debugEnabled = enabled)
                }
            } catch (e: Exception) {
                // Ignore
            }
        }
    }

    /**
     * 添加 HymoFS 规则
     */
    fun addHymoRule(src: String, target: String, type: Int = HymoFs.RuleType.FILE): Boolean {
        var success = false
        viewModelScope.launch {
            try {
                success = withContext(Dispatchers.IO) {
                    HymoFs.addRule(src, target, type)
                }
                if (success) {
                    refreshHymoFsStatus()
                }
            } catch (e: Exception) {
                // Ignore
            }
        }
        return success
    }

    /**
     * 清除所有 HymoFS 规则
     */
    fun clearHymoRules() {
        viewModelScope.launch {
            try {
                withContext(Dispatchers.IO) {
                    HymoFs.clearRules()
                }
                refreshHymoFsStatus()
            } catch (e: Exception) {
                // Ignore
            }
        }
    }

    /**
     * 检查 UID 是否有 root 权限
     */
    suspend fun checkUidHasRoot(uid: Int): Boolean {
        return withContext(Dispatchers.IO) {
            KsuService.isUidGrantedRoot(uid)
        }
    }

    /**
     * 执行 Paw Pad (清除 ext4 sysfs 痕迹)
     */
    fun nukeExt4Sysfs(): Boolean {
        var success = false
        viewModelScope.launch {
            try {
                success = withContext(Dispatchers.IO) {
                    KsuService.nukeExt4Sysfs()
                }
            } catch (e: Exception) {
                // Ignore
            }
        }
        return success
    }

    /**
     * 断开连接
     */
    fun disconnect() {
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                Murasaki.disconnect()
            }
            murasakiStatus = MurasakiStatus()
            hymoFsStatus = HymoFsStatus()
        }
    }

    override fun onCleared() {
        super.onCleared()
        Murasaki.disconnect()
    }

    private fun getPrivilegeLevelName(level: Int): String {
        return when (level) {
            Murasaki.LEVEL_SHELL -> "SHELL (Shizuku Compatible)"
            Murasaki.LEVEL_ROOT -> "ROOT (Sui Compatible)"
            Murasaki.LEVEL_KERNEL -> "KERNEL (Murasaki Exclusive)"
            else -> "Unknown ($level)"
        }
    }
}
