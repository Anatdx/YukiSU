package ui.screen.partition

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.*
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.rememberConfirmDialog
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.util.LocalSnackbarHost
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.*

/**
 * 分区管理界面
 * @author YukiSU
 */
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun PartitionManagerScreen(navigator: DestinationsNavigator) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbarHost = LocalSnackbarHost.current
    
    var partitionList by remember { mutableStateOf<List<PartitionInfo>>(emptyList()) }
    var slotInfo by remember { mutableStateOf<SlotInfo?>(null) }
    var isLoading by remember { mutableStateOf(true) }
    var selectedPartition by remember { mutableStateOf<PartitionInfo?>(null) }
    var showPartitionDialog by remember { mutableStateOf(false) }
    var pendingFlashPartition by remember { mutableStateOf<PartitionInfo?>(null) }
    
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())
    
    // 文件选择器
    val filePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent()
    ) { uri: Uri? ->
        uri?.let { selectedUri ->
            pendingFlashPartition?.let { partition ->
                scope.launch {
                    // 复制文件到缓存
                    val cacheFile = withContext(Dispatchers.IO) {
                        try {
                            val inputStream = context.contentResolver.openInputStream(selectedUri)
                            val tempFile = File(context.cacheDir, "flash_temp.img")
                            tempFile.outputStream().use { output ->
                                inputStream?.copyTo(output)
                            }
                            tempFile
                        } catch (e: Exception) {
                            null
                        }
                    }
                    
                    if (cacheFile != null) {
                        snackbarHost.showSnackbar("正在刷写 ${partition.name}...")
                        
                        withContext(Dispatchers.IO) {
                            val success = PartitionManagerHelper.flashPartition(
                                context = context,
                                imagePath = cacheFile.absolutePath,
                                partition = partition.name,
                                slot = slotInfo?.currentSlot,
                                onStdout = { line -> println("刷写: $line") },
                                onStderr = { line -> System.err.println("刷写错误: $line") }
                            )
                            
                            withContext(Dispatchers.Main) {
                                cacheFile.delete()
                                if (success) {
                                    snackbarHost.showSnackbar("刷写成功！建议立即重启。")
                                } else {
                                    snackbarHost.showSnackbar("刷写失败，请查看日志")
                                }
                            }
                        }
                    } else {
                        snackbarHost.showSnackbar("无法读取文件")
                    }
                }
            }
            pendingFlashPartition = null
        }
    }
    
    // 加载分区信息
    LaunchedEffect(Unit) {
        scope.launch {
            withContext(Dispatchers.IO) {
                isLoading = true
                try {
                    slotInfo = PartitionManagerHelper.getSlotInfo(context)
                    partitionList = PartitionManagerHelper.getPartitionList(context, slotInfo?.currentSlot)
                } catch (e: Exception) {
                    e.printStackTrace()
                } finally {
                    isLoading = false
                }
            }
        }
    }
    
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.partition_manager)) },
                navigationIcon = {
                    IconButton(onClick = { navigator.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
                    }
                },
                scrollBehavior = scrollBehavior,
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = if (CardConfig.isCustomBackgroundEnabled) {
                        MaterialTheme.colorScheme.surfaceContainerLow.copy(alpha = CardConfig.cardAlpha)
                    } else {
                        MaterialTheme.colorScheme.background.copy(alpha = CardConfig.cardAlpha)
                    }
                )
            )
        }
    ) { paddingValues ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues)
        ) {
            if (isLoading) {
                CircularProgressIndicator(
                    modifier = Modifier.align(Alignment.Center)
                )
            } else {
                LazyColumn(
                    modifier = Modifier
                        .fillMaxSize()
                        .nestedScroll(scrollBehavior.nestedScrollConnection),
                    contentPadding = PaddingValues(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    // 槽位信息卡片
                    if (slotInfo != null) {
                        item {
                            SlotInfoCard(slotInfo = slotInfo!!)
                        }
                    }
                    
                    // 分区列表
                    item {
                        Text(
                            text = stringResource(R.string.partition_list),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold,
                            modifier = Modifier.padding(top = 8.dp, bottom = 4.dp)
                        )
                    }
                    
                    items(partitionList) { partition ->
                        PartitionCard(
                            partition = partition,
                            onClick = {
                                selectedPartition = partition
                                showPartitionDialog = true
                            }
                        )
                    }
                }
            }
        }
    }
    
    // 分区操作对话框
    if (showPartitionDialog && selectedPartition != null) {
        PartitionActionDialog(
            partition = selectedPartition!!,
            currentSlot = slotInfo?.currentSlot,
            onDismiss = { showPartitionDialog = false },
            onBackup = {
                showPartitionDialog = false
                scope.launch {
                    handlePartitionBackup(context, selectedPartition!!, slotInfo?.currentSlot, snackbarHost)
                }
            },
            onFlash = {
                showPartitionDialog = false
                pendingFlashPartition = selectedPartition
                filePickerLauncher.launch("*/*")
            }
        )
    }
}

@Composable
fun SlotInfoCard(slotInfo: SlotInfo) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = getCardColors(),
        elevation = CardDefaults.cardElevation(defaultElevation = CardConfig.cardElevation)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = stringResource(R.string.slot_information),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )
                Icon(
                    imageVector = Icons.Filled.Info,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary
                )
            }
            
            Divider(modifier = Modifier.padding(vertical = 4.dp))
            
            if (slotInfo.isAbDevice) {
                InfoRow(
                    label = stringResource(R.string.device_type),
                    value = "A/B 设备"
                )
                InfoRow(
                    label = stringResource(R.string.current_slot),
                    value = slotInfo.currentSlot ?: "未知"
                )
                InfoRow(
                    label = stringResource(R.string.other_slot),
                    value = slotInfo.otherSlot ?: "未知"
                )
            } else {
                InfoRow(
                    label = stringResource(R.string.device_type),
                    value = "A-only 设备"
                )
            }
        }
    }
}

@Composable
fun PartitionCard(
    partition: PartitionInfo,
    onClick: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick),
        colors = getCardColors(),
        elevation = CardDefaults.cardElevation(defaultElevation = CardConfig.cardElevation)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Text(
                    text = partition.name,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = "${partition.type} • ${formatSize(partition.size)}",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                if (partition.blockDevice.isNotEmpty()) {
                    Text(
                        text = partition.blockDevice,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
            
            Icon(
                imageVector = if (partition.isLogical) Icons.Filled.Layers else Icons.Filled.Storage,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary
            )
        }
    }
}

@Composable
fun InfoRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Medium
        )
    }
}

@Composable
fun PartitionActionDialog(
    partition: PartitionInfo,
    currentSlot: String?,
    onDismiss: () -> Unit,
    onBackup: () -> Unit,
    onFlash: () -> Unit
) {
    val confirmDialog = rememberConfirmDialog(
        onConfirm = onFlash
    )
    
    AlertDialog(
        onDismissRequest = onDismiss,
        icon = {
            Icon(
                imageVector = if (partition.isLogical) Icons.Filled.Layers else Icons.Filled.Storage,
                contentDescription = null
            )
        },
        title = { Text(partition.name) },
        text = {
            Column(
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    text = "分区信息",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold
                )
                InfoRow(label = "类型", value = partition.type)
                InfoRow(label = "大小", value = formatSize(partition.size))
                if (partition.blockDevice.isNotEmpty()) {
                    InfoRow(label = "设备", value = partition.blockDevice)
                }
                if (currentSlot != null) {
                    InfoRow(label = "槽位", value = currentSlot)
                }
                
                Divider(modifier = Modifier.padding(vertical = 8.dp))
                
                Text(
                    text = "可用操作",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold
                )
                
                // 备份操作
                TextButton(
                    onClick = onBackup,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Filled.Download, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text("备份到文件")
                }
                
                // 刷写操作（警告色）
                TextButton(
                    onClick = {
                        confirmDialog.showConfirm(
                            title = "危险操作",
                            content = "刷写分区可能导致设备无法启动！\n\n确定要刷写 ${partition.name} 分区吗？",
                            confirm = "确定刷写"
                        )
                    },
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.textButtonColors(
                        contentColor = MaterialTheme.colorScheme.error
                    )
                ) {
                    Icon(Icons.Filled.Upload, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text("刷写镜像")
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(android.R.string.cancel))
            }
        }
    )
}

// 数据类
data class SlotInfo(
    val isAbDevice: Boolean,
    val currentSlot: String?,
    val otherSlot: String?
)

data class PartitionInfo(
    val name: String,
    val blockDevice: String,
    val type: String,
    val size: Long,
    val isLogical: Boolean
)

// 辅助函数
fun formatSize(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val kb = bytes / 1024.0
    if (kb < 1024) return String.format("%.2f KB", kb)
    val mb = kb / 1024.0
    if (mb < 1024) return String.format("%.2f MB", mb)
    val gb = mb / 1024.0
    return String.format("%.2f GB", gb)
}

suspend fun handlePartitionBackup(
    context: Context,
    partition: PartitionInfo,
    slot: String?,
    snackbarHost: SnackbarHostState
) {
    // 生成备份文件名
    val format = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault())
    val timestamp = format.format(Date())
    val fileName = "${partition.name}_$timestamp.img"
    
    val downloadsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
    val outputFile = File(downloadsDir, fileName)
    
    withContext(Dispatchers.Main) {
        snackbarHost.showSnackbar("正在备份分区 ${partition.name}...")
    }
    
    withContext(Dispatchers.IO) {
        val success = PartitionManagerHelper.backupPartition(
            context = context,
            partition = partition.name,
            outputPath = outputFile.absolutePath,
            slot = slot,
            onStdout = { line -> println("备份: $line") },
            onStderr = { line -> System.err.println("备份错误: $line") }
        )
        
        withContext(Dispatchers.Main) {
            if (success) {
                snackbarHost.showSnackbar("备份成功: $fileName")
            } else {
                snackbarHost.showSnackbar("备份失败，请查看日志")
            }
        }
    }
}
