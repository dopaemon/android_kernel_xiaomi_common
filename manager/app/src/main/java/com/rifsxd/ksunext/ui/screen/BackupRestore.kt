package com.rifsxd.ksunext.ui.screen

import android.content.Context
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Backup
import androidx.compose.material.icons.filled.Restore
import androidx.compose.material3.*
import androidx.compose.runtime.*
import android.app.Activity.RESULT_OK
import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import android.net.Uri
import android.os.Environment
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import com.rifsxd.ksunext.ui.LocalScrollState
import com.rifsxd.ksunext.ui.rememberScrollConnection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.dropUnlessResumed
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.ramcosta.composedestinations.navigation.EmptyDestinationsNavigator
import com.rifsxd.ksunext.Natives
import com.rifsxd.ksunext.R
import com.rifsxd.ksunext.ksuApp
import com.rifsxd.ksunext.ui.component.rememberLoadingDialog
import com.rifsxd.ksunext.ui.util.*
import kotlinx.coroutines.launch

/**
 * @author rifsxd
 * @date 2025/1/14.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun BackupRestoreScreen(navigator: DestinationsNavigator) {
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())
    val snackBarHost = LocalSnackbarHost.current

    val isManager = Natives.isManager
    val ksuVersion = if (isManager) Natives.version else null
    
    val navBarPadding = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding() + 112.dp

    Scaffold(
        topBar = {
            TopBar(
                onBack = dropUnlessResumed {
                    navigator.popBackStack()
                },
                scrollBehavior = scrollBehavior
            )
        },
        snackbarHost = { SnackbarHost(snackBarHost, modifier = Modifier.padding(bottom = navBarPadding)) },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
    ) { paddingValues ->
        val loadingDialog = rememberLoadingDialog()

        val context = LocalContext.current
        val scope = rememberCoroutineScope()

        // remember last requested filename for create-document launched backups
        val lastRequestedBackupName = rememberSaveable { mutableStateOf("") }

        val downloadsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)

        var showRebootDialog by remember { mutableStateOf(false) }
        // track which restore type user selected
        val lastRestoreType = rememberSaveable { mutableStateOf("module") }
        // track which backup type user selected
        val lastBackupType = rememberSaveable { mutableStateOf("module") }

        // CreateDocument launcher for backups (use file manager Intent like ModuleScreen)
        val createBackupLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.StartActivityForResult()
        ) { result ->
            if (result.resultCode != RESULT_OK) return@rememberLauncherForActivityResult
            val uri = result.data?.data ?: return@rememberLauncherForActivityResult
            scope.launch {
                loadingDialog.withLoading {
                    // create a temporary file under Downloads and ask ksu to copy there
                    val tmpPath = File(downloadsDir, lastRequestedBackupName.value).absolutePath
                    val ok = if (lastBackupType.value == "allowlist") {
                        allowlistBackupToExternal(tmpPath)
                    } else {
                        moduleBackupToExternal(tmpPath)
                    }
                    if (ok) {
                        // copy tmpPath -> user Uri
                        try {
                            FileInputStream(tmpPath).use { input ->
                                context.contentResolver.openOutputStream(uri)?.use { out ->
                                    input.copyTo(out)
                                }
                            }
                        } catch (e: Exception) {
                            e.printStackTrace()
                        }
                        // delete temporary file
                        File(tmpPath).delete()
                    }
                }
            }
        }

        // OpenDocument launcher for restores (use file manager Intent like ModuleScreen)
        val openRestoreLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.StartActivityForResult()
        ) { result ->
            if (result.resultCode != RESULT_OK) return@rememberLauncherForActivityResult
            val uri = result.data?.data ?: return@rememberLauncherForActivityResult
            scope.launch {
                loadingDialog.withLoading {
                    // copy the selected uri to Downloads and ask ksu to restore from there
                    val name = uri.getFileName(context)
                    val tmpFile = File(downloadsDir, name)
                    try {
                        context.contentResolver.openInputStream(uri)?.use { input ->
                            FileOutputStream(tmpFile).use { out ->
                                input.copyTo(out)
                            }
                        }
                    } catch (e: Exception) {
                        e.printStackTrace()
                    }
                    // choose correct restore function based on selected type
                    val resOk = if (lastRestoreType.value == "allowlist") {
                        allowlistRestoreFromExternalPath(tmpFile.absolutePath)
                    } else {
                        moduleRestoreFromExternalPath(tmpFile.absolutePath)
                    }
                    // if module restore succeeded, show reboot dialog
                    if (resOk && lastRestoreType.value == "module") {
                        showRebootDialog = true
                    }
                    tmpFile.delete()
                }
            }
        }

        

        Column(
            modifier = Modifier
                .padding(paddingValues)
                .let { modifier ->
                    val bottomBarScrollState = LocalScrollState.current
                    val bottomBarScrollConnection = if (bottomBarScrollState != null) {
                        rememberScrollConnection(
                            isScrollingDown = bottomBarScrollState.isScrollingDown,
                            scrollOffset = bottomBarScrollState.scrollOffset,
                            previousScrollOffset = bottomBarScrollState.previousScrollOffset,
                            threshold = 30f
                        )
                    } else null

                    if (bottomBarScrollConnection != null) {
                        modifier
                            .nestedScroll(bottomBarScrollConnection)
                            .nestedScroll(scrollBehavior.nestedScrollConnection)
                    } else {
                        modifier.nestedScroll(scrollBehavior.nestedScrollConnection)
                    }
                }
                .verticalScroll(rememberScrollState())
        ) {


            if (showRebootDialog) {
                AlertDialog(
                    onDismissRequest = { showRebootDialog = false },
                    title = { Text(
                        text = stringResource(R.string.reboot_required),
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.SemiBold
                    ) },
                    text = { Text(stringResource(R.string.reboot_message)) },
                    confirmButton = {
                        TextButton(onClick = {
                            showRebootDialog = false
                            reboot()
                        }) {
                            Text(stringResource(R.string.reboot))
                        }
                    },
                    dismissButton = {
                        TextButton(onClick = { showRebootDialog = false }) {
                            Text(stringResource(R.string.later))
                        }
                    }
                )
            }

            val moduleBackup = stringResource(id = R.string.module_backup)
            val backupMessage = stringResource(id = R.string.module_backup_message)
            ListItem(
                leadingContent = {
                    Icon(
                        Icons.Filled.Backup,
                        moduleBackup
                    )
                },
                headlineContent = { Text(
                    text = moduleBackup,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                ) },
                modifier = Modifier.clickable {
                        scope.launch {
                            // prepare filename and launch file manager create document
                            val timestamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.getDefault()).format(java.util.Date())
                            val suggested = "modules_backup_$timestamp.tar"
                            lastRequestedBackupName.value = suggested
                            lastBackupType.value = "module"
                            val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                                addCategory(Intent.CATEGORY_OPENABLE)
                                type = "application/x-tar"
                                putExtra(Intent.EXTRA_TITLE, suggested)
                            }
                            createBackupLauncher.launch(intent)
                        }
                }
            )

            if (showRebootDialog) {
                AlertDialog(
                    onDismissRequest = { showRebootDialog = false },
                    title = { Text(
                        text = stringResource(R.string.reboot_required),
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.SemiBold
                    ) },
                    text = { Text(stringResource(R.string.reboot_message)) },
                    confirmButton = {
                        TextButton(onClick = {
                            showRebootDialog = false
                            reboot()
                        }) {
                            Text(stringResource(R.string.reboot))
                        }
                    },
                    dismissButton = {
                        TextButton(onClick = { showRebootDialog = false }) {
                            Text(stringResource(R.string.later))
                        }
                    }
                )
            }

            val moduleRestore = stringResource(id = R.string.module_restore)
            val restoreMessage = stringResource(id = R.string.module_restore_message)

            ListItem(
                leadingContent = {
                    Icon(
                        Icons.Filled.Restore,
                        moduleRestore,
                        tint = MaterialTheme.colorScheme.onSurface
                    )
                },
                headlineContent = { 
                    Text(
                        moduleRestore,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                        color = MaterialTheme.colorScheme.onSurface
                    ) 
                },
                modifier = Modifier.clickable(
                    onClick = {
                        scope.launch {
                            lastRestoreType.value = "module"
                            val intent = Intent(Intent.ACTION_GET_CONTENT).apply {
                                addCategory(Intent.CATEGORY_OPENABLE)
                                type = "application/x-tar"
                            }
                            openRestoreLauncher.launch(intent)
                        }
                    }
                )
            )

            HorizontalDivider(thickness = Dp.Hairline)

            val allowlistBackup = stringResource(id = R.string.allowlist_backup)
            val allowlistbackupMessage = stringResource(id = R.string.allowlist_backup_message)
            ListItem(
                leadingContent = {
                    Icon(
                        Icons.Filled.Backup,
                        allowlistBackup
                    )
                },
                headlineContent = { Text(
                    text = allowlistBackup,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                ) },
                modifier = Modifier.clickable {
                        scope.launch {
                            val timestamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.getDefault()).format(java.util.Date())
                            val suggested = "allowlist_backup_$timestamp.tar"
                            lastRequestedBackupName.value = suggested
                            lastBackupType.value = "allowlist"
                            val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                                addCategory(Intent.CATEGORY_OPENABLE)
                                type = "application/x-tar"
                                putExtra(Intent.EXTRA_TITLE, suggested)
                            }
                            createBackupLauncher.launch(intent)
                        }
                }
            )

            val allowlistRestore = stringResource(id = R.string.allowlist_restore)
            val allowlistrestoreMessage = stringResource(id = R.string.allowlist_restore_message)
            ListItem(
                leadingContent = {
                    Icon(
                        Icons.Filled.Restore,
                        allowlistRestore
                    )
                },
                headlineContent = { Text(
                    text = allowlistRestore,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                ) },
                modifier = Modifier.clickable {
                        scope.launch {
                            lastRestoreType.value = "allowlist"
                            val intent = Intent(Intent.ACTION_GET_CONTENT).apply {
                                addCategory(Intent.CATEGORY_OPENABLE)
                                type = "application/x-tar"
                            }
                            openRestoreLauncher.launch(intent)
                        }
                }
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    onBack: () -> Unit = {},
    scrollBehavior: TopAppBarScrollBehavior? = null
) {
    TopAppBar(
        title = { Text(
                text = stringResource(R.string.backup_restore),
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.Black,
            ) }, navigationIcon = {
            IconButton(
                onClick = onBack
            ) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null) }
        },
        windowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
        scrollBehavior = scrollBehavior
    )
}

@Preview
@Composable
private fun BackupPreview() {
    BackupRestoreScreen(EmptyDestinationsNavigator)
}
