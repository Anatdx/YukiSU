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
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
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
    var allPartitionList by remember { mutableStateOf<List<PartitionInfo>>(emptyList()) }
    var slotInfo by remember { mutableStateOf<SlotInfo?>(null) }
    var isLoading by remember { mutableStateOf(true) }
    var selectedPartition by remember { mutableStateOf<PartitionInfo?>(null) }
    var showPartitionDialog by remember { mutableStateOf(false) }
    var pendingFlashPartition by remember { mutableStateOf<PartitionInfo?>(null) }
    var showAllPartitions by remember { mutableStateOf(false) }
    var multiSelectMode by remember { mutableStateOf(false) }
    var selectedPartitions by remember { mutableStateOf<Set<String>>(emptySet()) }
    var selectedSlot by remember { mutableStateOf<String?>(null) }  // 新增：选中的槽位
    
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
                        snackbarHost.showSnackbar(context.getString(R.string.partition_flashing, partition.name))
                        
                        withContext(Dispatchers.IO) {
                            val logs = mutableListOf<String>()
                            val success = PartitionManagerHelper.flashPartition(
                                context = context,
                                imagePath = cacheFile.absolutePath,
                                partition = partition.name,
                                slot = slotInfo?.currentSlot,
                                onStdout = { line -> 
                                    android.util.Log.d("PartitionFlash", "stdout: $line")
                                    logs.add(line)
                                },
                                onStderr = { line -> 
                                    android.util.Log.e("PartitionFlash", "stderr: $line")
                                    logs.add("ERROR: $line")
                                }
                            )
                            
                            withContext(Dispatchers.Main) {
                                cacheFile.delete()
                                if (success) {
                                    snackbarHost.showSnackbar(context.getString(R.string.partition_flash_success))
                                } else {
                                    val errorMsg = if (logs.isNotEmpty()) {
                                        context.getString(R.string.partition_flash_failed, logs.lastOrNull() ?: context.getString(R.string.partition_unknown))
                                    } else {
                                        context.getString(R.string.partition_flash_failed_check_log)
                                    }
                                    snackbarHost.showSnackbar(errorMsg)
                                }
                            }
                        }
                    } else {
                        snackbarHost.showSnackbar(context.getString(R.string.partition_cannot_read_file))
                    }
                }
            }
            pendingFlashPartition = null
        }
    }
    
    // 刷新分区列表函数
    val refreshPartitions: suspend (String?) -> Unit = { slot ->
        withContext(Dispatchers.IO) {
            isLoading = true
            try {
                partitionList = PartitionManagerHelper.getPartitionList(context, slot, scanAll = false)
                allPartitionList = PartitionManagerHelper.getPartitionList(context, slot, scanAll = true)
            } catch (e: Exception) {
                android.util.Log.e("PartitionManager", "Failed to refresh partitions", e)
            } finally {
                isLoading = false
            }
        }
    }
    
    // 加载分区信息
    LaunchedEffect(Unit) {
        scope.launch {
            withContext(Dispatchers.IO) {
                isLoading = true
                try {
                    android.util.Log.d("PartitionManager", "Starting to load partition info...")
                    slotInfo = PartitionManagerHelper.getSlotInfo(context)
                    android.util.Log.d("PartitionManager", "Slot info loaded: isAB=${slotInfo?.isAbDevice}, current=${slotInfo?.currentSlot}")
                    
                    // 默认选择当前槽位
                    selectedSlot = slotInfo?.currentSlot
                    
                    // 加载常用分区
                    partitionList = PartitionManagerHelper.getPartitionList(context, selectedSlot, scanAll = false)
                    android.util.Log.d("PartitionManager", "Loaded ${partitionList.size} common partitions")
                    
                    // 加载所有分区
                    allPartitionList = PartitionManagerHelper.getPartitionList(context, selectedSlot, scanAll = true)
                    android.util.Log.d("PartitionManager", "Loaded ${allPartitionList.size} total partitions")
                    
                    partitionList.forEach { p ->
                        android.util.Log.d("PartitionManager", "  ${p.name}: ${p.size} bytes, ${p.type}, ${p.blockDevice}, dangerous=${p.isDangerous}")
                    }
                } catch (e: Exception) {
                    android.util.Log.e("PartitionManager", "Failed to load partition info", e)
                    e.printStackTrace()
                    withContext(Dispatchers.Main) {
                        snackbarHost.showSnackbar(context.getString(R.string.partition_load_failed, e.message ?: ""))
                    }
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
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.partition_back))
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
                    // 槽位信息卡片和槽位选择器
                    if (slotInfo != null) {
                        item {
                            SlotInfoCard(
                                slotInfo = slotInfo!!,
                                selectedSlot = selectedSlot,
                                onSlotChange = { newSlot ->
                                    selectedSlot = newSlot
                                    scope.launch {
                                        refreshPartitions(newSlot)
                                    }
                                }
                            )
                        }
                    }
                    
                    // 操作按钮行
                    item {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            // 展开/收起按钮
                            OutlinedButton(
                                onClick = { showAllPartitions = !showAllPartitions },
                                modifier = Modifier.weight(1f)
                            ) {
                                Icon(
                                    if (showAllPartitions) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                                    contentDescription = null,
                                    modifier = Modifier.size(18.dp)
                                )
                                Spacer(Modifier.width(4.dp))
                                Text(stringResource(
                                    if (showAllPartitions) R.string.partition_collapse 
                                    else R.string.partition_show_all
                                ))
                            }
                        }
                    }
                    
                    // 多选模式下的批量操作按钮
                    if (multiSelectMode && selectedPartitions.isNotEmpty()) {
                        item {
                            Card(
                                colors = CardDefaults.cardColors(
                                    containerColor = MaterialTheme.colorScheme.primaryContainer
                                )
                            ) {
                                Row(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .padding(12.dp),
                                    horizontalArrangement = Arrangement.SpaceBetween,
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Text(
                                        stringResource(R.string.partition_selected_count, selectedPartitions.size),
                                        style = MaterialTheme.typography.bodyMedium
                                    )
                                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                        // 全选按钮（排除逻辑分区和不可备份分区）
                                        IconButton(onClick = {
                                            val displayList = if (showAllPartitions) allPartitionList else partitionList
                                            val selectablePartitions = displayList.filterNot { 
                                                it.excludeFromBatch || it.isLogical 
                                            }
                                            selectedPartitions = selectablePartitions.map { it.name }.toSet()
                                        }) {
                                            Icon(
                                                Icons.Default.CheckCircle,
                                                contentDescription = stringResource(R.string.partition_select_all)
                                            )
                                        }
                                        // 全不选按钮
                                        IconButton(onClick = {
                                            selectedPartitions = emptySet()
                                        }) {
                                            Icon(
                                                Icons.Default.Cancel,
                                                contentDescription = stringResource(R.string.partition_deselect_all)
                                            )
                                        }
                                        Button(onClick = {
                                            scope.launch {
                                                handleBatchBackup(
                                                    context,
                                                    selectedPartitions,
                                                    if (showAllPartitions) allPartitionList else partitionList,
                                                    snackbarHost
                                                )
                                            }
                                        }) {
                                            Icon(Icons.Default.Download, null, Modifier.size(18.dp))
                                            Spacer(Modifier.width(4.dp))
                                            Text(stringResource(R.string.partition_batch_backup))
                                        }
                                    }
                                }
                            }
                        }
                    }
                    
                    // 分区列表标题
                    item {
                        Text(
                            text = stringResource(
                                if (showAllPartitions) R.string.partition_all 
                                else R.string.partition_common
                            ),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold,
                            modifier = Modifier.padding(top = 8.dp, bottom = 4.dp)
                        )
                    }
                    
                    // 分区列表
                    val displayList = if (showAllPartitions) allPartitionList else partitionList
                    items(displayList) { partition ->
                        PartitionCard(
                            partition = partition,
                            isSelected = selectedPartitions.contains(partition.name),
                            multiSelectMode = multiSelectMode,
                            onClick = {
                                if (multiSelectMode) {
                                    selectedPartitions = if (selectedPartitions.contains(partition.name)) {
                                        selectedPartitions - partition.name
                                    } else {
                                        selectedPartitions + partition.name
                                    }
                                } else {
                                    selectedPartition = partition
                                    showPartitionDialog = true
                                }
                            },
                            onLongClick = {
                                // 长按进入多选模式并选中当前项
                                if (!multiSelectMode) {
                                    multiSelectMode = true
                                }
                                selectedPartitions = if (selectedPartitions.contains(partition.name)) {
                                    selectedPartitions - partition.name
                                } else {
                                    selectedPartitions + partition.name
                                }
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
fun SlotInfoCard(
    slotInfo: SlotInfo,
    selectedSlot: String?,
    onSlotChange: (String?) -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
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
                    text = stringResource(R.string.partition_slot_info),
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
                    label = stringResource(R.string.partition_device_type),
                    value = stringResource(R.string.partition_ab_device)
                )
                
                // 槽位切换按钮
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    // Current Slot 按钮
                    FilterChip(
                        selected = selectedSlot == slotInfo.currentSlot,
                        onClick = { onSlotChange(slotInfo.currentSlot) },
                        label = { 
                            Text("${stringResource(R.string.partition_current_slot)}: ${slotInfo.currentSlot ?: stringResource(R.string.partition_unknown)}") 
                        },
                        leadingIcon = if (selectedSlot == slotInfo.currentSlot) {
                            { Icon(Icons.Default.Check, contentDescription = null, Modifier.size(18.dp)) }
                        } else null,
                        modifier = Modifier.weight(1f)
                    )
                    
                    // Other Slot 按钮
                    FilterChip(
                        selected = selectedSlot == slotInfo.otherSlot,
                        onClick = { onSlotChange(slotInfo.otherSlot) },
                        label = { 
                            Text("${stringResource(R.string.partition_other_slot)}: ${slotInfo.otherSlot ?: stringResource(R.string.partition_unknown)}") 
                        },
                        leadingIcon = if (selectedSlot == slotInfo.otherSlot) {
                            { Icon(Icons.Default.Check, contentDescription = null, Modifier.size(18.dp)) }
                        } else null,
                        modifier = Modifier.weight(1f)
                    )
                }
            } else {
                InfoRow(
                    label = stringResource(R.string.partition_device_type),
                    value = stringResource(R.string.partition_a_only_device)
                )
            }
        }
    }
}

@Composable
fun PartitionCard(
    partition: PartitionInfo,
    isSelected: Boolean = false,
    multiSelectMode: Boolean = false,
    onClick: () -> Unit,
    onLongClick: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .combinedClickable(
                onClick = onClick,
                onLongClick = onLongClick
            ),
        colors = if (isSelected) {
            CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)
        } else {
            getCardColors(MaterialTheme.colorScheme.surfaceContainerLow)
        },
        elevation = CardDefaults.cardElevation(defaultElevation = CardConfig.cardElevation),
        border = if (partition.isDangerous) {
            BorderStroke(2.dp, MaterialTheme.colorScheme.error)
        } else null
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            // 多选模式下的复选框
            if (multiSelectMode) {
                Checkbox(
                    checked = isSelected,
                    onCheckedChange = { onClick() },
                    enabled = !partition.excludeFromBatch,
                    modifier = Modifier.padding(end = 8.dp)
                )
            }
            
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = partition.name,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                    if (partition.isDangerous) {
                        Spacer(Modifier.width(4.dp))
                        Icon(
                            Icons.Default.Warning,
                            contentDescription = stringResource(R.string.partition_dangerous_warning),
                            tint = MaterialTheme.colorScheme.error,
                            modifier = Modifier.size(18.dp)
                        )
                    }
                    if (partition.excludeFromBatch) {
                        Spacer(Modifier.width(4.dp))
                        Icon(
                            Icons.Default.Block,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.outline,
                            modifier = Modifier.size(16.dp)
                        )
                    }
                }
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
                tint = if (isSelected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant
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
    val context = LocalContext.current
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
                    text = stringResource(R.string.partition_info_title),
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold
                )
                InfoRow(label = stringResource(R.string.partition_info_type), value = partition.type)
                InfoRow(label = stringResource(R.string.partition_info_size), value = formatSize(partition.size))
                if (partition.blockDevice.isNotEmpty()) {
                    InfoRow(label = stringResource(R.string.partition_info_device), value = partition.blockDevice)
                }
                if (currentSlot != null) {
                    InfoRow(label = stringResource(R.string.partition_info_slot), value = currentSlot)
                }
                
                // 危险分区警告
                if (partition.isDangerous) {
                    Card(
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.errorContainer
                        ),
                        modifier = Modifier.padding(vertical = 8.dp)
                    ) {
                        Row(
                            modifier = Modifier.padding(12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(
                                Icons.Default.Warning,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.error
                            )
                            Spacer(Modifier.width(8.dp))
                            Text(
                                text = stringResource(R.string.partition_dangerous_warning),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onErrorContainer
                            )
                        }
                    }
                } else {
                    Divider(modifier = Modifier.padding(vertical = 8.dp))
                }
                
                Text(
                    text = stringResource(R.string.partition_available_operations),
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold
                )
                
                // 备份操作
                TextButton(
                    onClick = onBackup,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Filled.Upload, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.partition_backup_to_file))
                }
                
                // 刷写操作（警告色）
                TextButton(
                    onClick = {
                        val warningMessage = if (partition.isDangerous) {
                            context.getString(R.string.partition_dangerous_flash_warning, partition.name, partition.name)
                        } else {
                            context.getString(R.string.partition_flash_warning, partition.name)
                        }
                        
                        confirmDialog.showConfirm(
                            title = context.getString(
                                if (partition.isDangerous) R.string.partition_dangerous_operation_warning 
                                else R.string.partition_dangerous_operation
                            ),
                            content = warningMessage,
                            confirm = context.getString(R.string.partition_confirm_flash)
                        )
                    },
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.textButtonColors(
                        contentColor = MaterialTheme.colorScheme.error
                    )
                ) {
                    Icon(Icons.Filled.Download, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.partition_flash_image))
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
    val isLogical: Boolean,
    val isDangerous: Boolean = false,
    val excludeFromBatch: Boolean = false
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
        snackbarHost.showSnackbar(context.getString(R.string.partition_backing_up, partition.name))
    }
    
    withContext(Dispatchers.IO) {
        val logs = mutableListOf<String>()
        val success = PartitionManagerHelper.backupPartition(
            context = context,
            partition = partition.name,
            outputPath = outputFile.absolutePath,
            slot = slot,
            onStdout = { line -> 
                android.util.Log.d("PartitionBackup", "stdout: $line")
                logs.add(line)
            },
            onStderr = { line -> 
                android.util.Log.e("PartitionBackup", "stderr: $line")
                logs.add("ERROR: $line")
            }
        )
        
        withContext(Dispatchers.Main) {
            if (success) {
                snackbarHost.showSnackbar(context.getString(R.string.partition_backup_success, fileName))
            } else {
                val errorMsg = if (logs.isNotEmpty()) {
                    context.getString(R.string.partition_backup_failed, logs.lastOrNull() ?: context.getString(R.string.partition_unknown))
                } else {
                    context.getString(R.string.partition_backup_failed_check_log)
                }
                snackbarHost.showSnackbar(errorMsg)
            }
        }
    }
}

/**
 * 批量备份分区
 */
suspend fun handleBatchBackup(
    context: Context,
    selectedPartitionNames: Set<String>,
    allPartitions: List<PartitionInfo>,
    snackbarHost: SnackbarHostState
) {
    val partitionsToBackup = allPartitions.filter { it.name in selectedPartitionNames }
    
    if (partitionsToBackup.isEmpty()) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(context.getString(R.string.partition_no_selection))
        }
        return
    }
    
    // 生成备份目录
    val format = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault())
    val timestamp = format.format(Date())
    val backupDirName = "partition_backup_$timestamp"
    val downloadsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
    val backupDir = File(downloadsDir, backupDirName)
    
    if (!backupDir.exists()) {
        backupDir.mkdirs()
    }
    
    withContext(Dispatchers.Main) {
        snackbarHost.showSnackbar(context.getString(R.string.partition_batch_backup_start, partitionsToBackup.size))
    }
    
    var successCount = 0
    var failedPartitions = mutableListOf<String>()
    
    for ((index, partition) in partitionsToBackup.withIndex()) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(context.getString(R.string.partition_batch_backup_progress, index + 1, partitionsToBackup.size, partition.name))
        }
        
        val outputFile = File(backupDir, "${partition.name}.img")
        
        withContext(Dispatchers.IO) {
            val logs = mutableListOf<String>()
            val success = PartitionManagerHelper.backupPartition(
                context = context,
                partition = partition.name,
                outputPath = outputFile.absolutePath,
                slot = null,
                onStdout = { line -> 
                    android.util.Log.d("BatchBackup", "[${partition.name}] stdout: $line")
                    logs.add(line)
                },
                onStderr = { line -> 
                    android.util.Log.e("BatchBackup", "[${partition.name}] stderr: $line")
                    logs.add("ERROR: $line")
                }
            )
            
            if (success) {
                successCount++
            } else {
                failedPartitions.add(partition.name)
            }
        }
    }
    
    withContext(Dispatchers.Main) {
        if (failedPartitions.isEmpty()) {
            snackbarHost.showSnackbar(context.getString(R.string.partition_batch_backup_complete, successCount, backupDirName))
        } else {
            snackbarHost.showSnackbar(context.getString(R.string.partition_batch_backup_partial, successCount, failedPartitions.size, failedPartitions.joinToString()))
        }
    }
}
