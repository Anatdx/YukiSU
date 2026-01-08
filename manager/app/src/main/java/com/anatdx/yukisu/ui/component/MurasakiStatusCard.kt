package com.anatdx.yukisu.ui.component

import androidx.compose.animation.*
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.viewmodel.MurasakiViewModel
import io.murasaki.Murasaki

/**
 * Murasaki 服务状态卡片
 * 
 * 显示 Murasaki API 的连接状态、权限等级和功能快捷操作
 */
@Composable
fun MurasakiStatusCard(
    viewModel: MurasakiViewModel,
    modifier: Modifier = Modifier
) {
    val status = viewModel.murasakiStatus
    val hymoFsStatus = viewModel.hymoFsStatus
    val isLoading = viewModel.isLoading

    // 自动连接
    LaunchedEffect(Unit) {
        viewModel.connect()
    }

    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = getCardColors(),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Column(
            modifier = Modifier.padding(16.dp)
        ) {
            // 标题行
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = Icons.Default.Hub,
                        contentDescription = "Murasaki",
                        tint = MaterialTheme.colorScheme.primary
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = "Murasaki API",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                }

                // 连接状态指示器
                ConnectionIndicator(
                    isConnected = status.isConnected,
                    isLoading = isLoading
                )
            }

            Spacer(modifier = Modifier.height(16.dp))

            if (isLoading) {
                Box(
                    modifier = Modifier.fillMaxWidth(),
                    contentAlignment = Alignment.Center
                ) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(32.dp),
                        strokeWidth = 3.dp
                    )
                }
            } else if (status.isConnected) {
                // 连接成功显示详情
                MurasakiDetails(status, hymoFsStatus, viewModel)
            } else {
                // 连接失败
                ConnectionError(status.error, onRetry = { viewModel.connect() })
            }
        }
    }
}

@Composable
private fun ConnectionIndicator(isConnected: Boolean, isLoading: Boolean) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Box(
            modifier = Modifier
                .size(10.dp)
                .clip(CircleShape)
                .background(
                    when {
                        isLoading -> Color.Yellow
                        isConnected -> Color.Green
                        else -> Color.Red
                    }
                )
        )
        Text(
            text = when {
                isLoading -> "连接中..."
                isConnected -> "已连接"
                else -> "未连接"
            },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun MurasakiDetails(
    status: MurasakiViewModel.MurasakiStatus,
    hymoFsStatus: MurasakiViewModel.HymoFsStatus,
    viewModel: MurasakiViewModel
) {
    Column(
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // 权限等级
        PrivilegeLevelBadge(status.privilegeLevel, status.privilegeLevelName)

        // 版本信息
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            InfoItem(
                icon = Icons.Default.Info,
                label = "服务版本",
                value = if (status.serviceVersion >= 0) "v${status.serviceVersion}" else "N/A"
            )
            InfoItem(
                icon = Icons.Default.Security,
                label = "KSU 版本",
                value = if (status.ksuVersion >= 0) status.ksuVersion.toString() else "N/A"
            )
        }

        // SELinux 上下文
        status.selinuxContext?.let { ctx ->
            InfoRow(
                icon = Icons.Default.Shield,
                label = "SELinux",
                value = ctx,
                mono = true
            )
        }

        // 内核模式状态
        InfoRow(
            icon = Icons.Default.Memory,
            label = "内核模式",
            value = if (status.isKernelModeAvailable) "可用" else "不可用"
        )

        HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))

        // 快捷操作
        Text(
            text = "快捷操作",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.primary
        )

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            ActionChip(
                icon = Icons.Default.Visibility,
                label = "隐身",
                isActive = hymoFsStatus.stealthEnabled == true,
                onClick = { viewModel.setStealthMode(hymoFsStatus.stealthEnabled != true) }
            )
            ActionChip(
                icon = Icons.Default.BugReport,
                label = "调试",
                isActive = hymoFsStatus.debugEnabled == true,
                onClick = { viewModel.setDebugMode(hymoFsStatus.debugEnabled != true) }
            )
            ActionChip(
                icon = Icons.Default.CleaningServices,
                label = "Paw Pad",
                isActive = false,
                onClick = { viewModel.nukeExt4Sysfs() }
            )
        }

        // 规则统计
        hymoFsStatus.activeRules?.let { rules ->
            val ruleCount = rules.lines().filter { it.isNotBlank() }.size
            InfoRow(
                icon = Icons.Default.Rule,
                label = "活跃规则",
                value = "$ruleCount 条"
            )
        }
    }
}

@Composable
private fun PrivilegeLevelBadge(level: Int, name: String) {
    val (color, icon) = when (level) {
        Murasaki.LEVEL_SHELL -> Pair(Color(0xFF4CAF50), Icons.Default.Terminal)
        Murasaki.LEVEL_ROOT -> Pair(Color(0xFFFF9800), Icons.Default.AdminPanelSettings)
        Murasaki.LEVEL_KERNEL -> Pair(Color(0xFFF44336), Icons.Default.Memory)
        else -> Pair(Color.Gray, Icons.Default.Help)
    }

    Surface(
        shape = RoundedCornerShape(8.dp),
        color = color.copy(alpha = 0.1f),
        modifier = Modifier.fillMaxWidth()
    ) {
        Row(
            modifier = Modifier.padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = color,
                modifier = Modifier.size(24.dp)
            )
            Column {
                Text(
                    text = "权限等级",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    text = name,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Bold,
                    color = color
                )
            }
        }
    }
}

@Composable
private fun InfoItem(icon: ImageVector, label: String, value: String) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.primary,
            modifier = Modifier.size(20.dp)
        )
        Spacer(modifier = Modifier.height(4.dp))
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Bold
        )
    }
}

@Composable
private fun InfoRow(
    icon: ImageVector,
    label: String,
    value: String,
    mono: Boolean = false
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(18.dp)
            )
            Text(
                text = label,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Text(
            text = value,
            style = MaterialTheme.typography.bodySmall,
            fontFamily = if (mono) FontFamily.Monospace else null,
            fontWeight = FontWeight.Medium
        )
    }
}

@Composable
private fun ActionChip(
    icon: ImageVector,
    label: String,
    isActive: Boolean,
    onClick: () -> Unit
) {
    FilterChip(
        selected = isActive,
        onClick = onClick,
        label = { Text(label, fontSize = 12.sp) },
        leadingIcon = {
            Icon(
                imageVector = icon,
                contentDescription = null,
                modifier = Modifier.size(16.dp)
            )
        },
        modifier = Modifier.height(32.dp)
    )
}

@Composable
private fun ConnectionError(error: String?, onRetry: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Icon(
            imageVector = Icons.Default.Error,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.error,
            modifier = Modifier.size(48.dp)
        )
        Text(
            text = "连接失败",
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.error
        )
        error?.let {
            Text(
                text = it,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        OutlinedButton(onClick = onRetry) {
            Icon(Icons.Default.Refresh, contentDescription = null)
            Spacer(modifier = Modifier.width(8.dp))
            Text("重试")
        }
    }
}
