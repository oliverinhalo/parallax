package uk.co.jacoblevy.parallax.feature.library

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Snackbar
import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import uk.co.jacoblevy.parallax.core.common.util.formatBytes
import uk.co.jacoblevy.parallax.core.designsystem.component.EmptyState
import uk.co.jacoblevy.parallax.core.designsystem.component.ErrorState
import uk.co.jacoblevy.parallax.core.designsystem.component.LoadingState
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxChip
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxIconButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxTextButton
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.LibrarySort
import uk.co.jacoblevy.parallax.core.model.Project

/**
 * The library: every scan on the device.
 *
 * This is the app's home, so it carries the two things the user came for -- start a scan, open a
 * scan -- and keeps everything else (folders, sorting, bulk actions) one tap away rather than
 * on screen at all times.
 */
@Composable
fun LibraryRoute(
    onOpenProject: (String) -> Unit,
    onStartCapture: () -> Unit,
    onOpenSettings: () -> Unit,
    onOpenQueue: () -> Unit,
    viewModel: LibraryViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    LibraryScreen(
        state = state,
        onEvent = viewModel::onEvent,
        onOpenProject = onOpenProject,
        onStartCapture = onStartCapture,
        onOpenSettings = onOpenSettings,
        onOpenQueue = onOpenQueue,
        thumbnailFor = viewModel::thumbnailFor,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun LibraryScreen(
    state: LibraryUiState,
    onEvent: (LibraryEvent) -> Unit,
    onOpenProject: (String) -> Unit,
    onStartCapture: () -> Unit,
    onOpenSettings: () -> Unit,
    onOpenQueue: () -> Unit,
    thumbnailFor: (Project) -> java.io.File?,
) {
    val snackbarHost = remember { SnackbarHostState() }

    val picker = rememberLauncherForActivityResult(
        // The MIME type of a .parallax bundle is a ZIP, and many file providers report it as
        // octet-stream, so both are accepted rather than the file being un-pickable.
        contract = ActivityResultContracts.OpenDocument(),
        onResult = { uri -> uri?.let { onEvent(LibraryEvent.ImportBundle(it)) } },
    )

    LaunchedEffect(state.notice) {
        val notice = state.notice ?: return@LaunchedEffect
        snackbarHost.showSnackbar(message = notice, duration = SnackbarDuration.Short)
        onEvent(LibraryEvent.DismissNotice)
    }

    Scaffold(
        containerColor = ParallaxTheme.colors.surfaceSunken,
        topBar = {
            if (state.inSelectionMode) {
                SelectionTopBar(state, onEvent)
            } else {
                LibraryTopBar(
                    state = state,
                    onEvent = onEvent,
                    onOpenSettings = onOpenSettings,
                    onOpenQueue = onOpenQueue,
                    onImport = { picker.launch(BUNDLE_MIME_TYPES) },
                )
            }
        },
        snackbarHost = {
            SnackbarHost(snackbarHost) { data ->
                Snackbar(
                    snackbarData = data,
                    containerColor = ParallaxTheme.colors.surfaceOverlay,
                    contentColor = ParallaxTheme.colors.textPrimary,
                )
            }
        },
        floatingActionButton = {
            if (!state.inSelectionMode) {
                NewScanButton(onClick = onStartCapture)
            }
        },
    ) { padding ->
        Box(Modifier.padding(padding).fillMaxSize()) {
            when {
                state.loading -> LoadingState(message = "Reading your library")
                state.error != null -> ErrorState(
                    error = state.error,
                    onRecover = { onEvent(LibraryEvent.DismissError) },
                )
                state.isEmpty -> EmptyState(
                    title = "No scans yet",
                    message = "Point your phone at something and walk around it. " +
                        "Everything stays on this device.",
                    icon = ParallaxIcons.Scan,
                    actionLabel = "Start a scan",
                    onAction = onStartCapture,
                )
                else -> LibraryContent(
                    state = state,
                    onEvent = onEvent,
                    onOpenProject = onOpenProject,
                    thumbnailFor = thumbnailFor,
                )
            }
        }
    }

    LibraryDialogs(state = state, onEvent = onEvent)
}

@Composable
private fun LibraryContent(
    state: LibraryUiState,
    onEvent: (LibraryEvent) -> Unit,
    onOpenProject: (String) -> Unit,
    thumbnailFor: (Project) -> java.io.File?,
) {
    Column(Modifier.fillMaxSize()) {
        FilterRow(state, onEvent)
        if (state.isFilteredEmpty) {
            EmptyState(
                title = "Nothing matches",
                message = "No scan matches the current search and filters.",
                icon = ParallaxIcons.Filter,
                actionLabel = "Clear filters",
                onAction = { onEvent(LibraryEvent.ClearFilters) },
            )
            return@Column
        }
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = MIN_CARD_WIDTH.dp),
            contentPadding = PaddingValues(
                start = Spacing.screenHorizontal,
                end = Spacing.screenHorizontal,
                top = Spacing.md,
                // Clear the floating action button and the navigation bar, so the last row is
                // not permanently hidden behind them.
                bottom = Spacing.thumbReach +
                    WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding(),
            ),
            horizontalArrangement = Arrangement.spacedBy(Spacing.md),
            verticalArrangement = Arrangement.spacedBy(Spacing.md),
            modifier = Modifier.fillMaxSize(),
        ) {
            items(items = state.projects, key = { it.id }) { project ->
                ProjectCard(
                    project = project,
                    thumbnail = thumbnailFor(project),
                    folderName = state.folderNameFor(project),
                    selected = project.id in state.selection,
                    selectionMode = state.inSelectionMode,
                    onOpen = { onOpenProject(project.id) },
                    onToggleSelected = { onEvent(LibraryEvent.ToggleSelected(project.id)) },
                    onToggleFavourite = { onEvent(LibraryEvent.ToggleFavourite(project.id)) },
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LibraryTopBar(
    state: LibraryUiState,
    onEvent: (LibraryEvent) -> Unit,
    onOpenSettings: () -> Unit,
    onOpenQueue: () -> Unit,
    onImport: () -> Unit,
) {
    var searching by remember { mutableStateOf(false) }
    Column {
        TopAppBar(
            title = {
                if (searching) {
                    SearchField(
                        value = state.query.search,
                        onValueChange = { onEvent(LibraryEvent.SearchChanged(it)) },
                        onDone = { if (state.query.search.isBlank()) searching = false },
                    )
                } else {
                    Column {
                        Text("Library", style = MaterialTheme.typography.titleLarge)
                        Text(
                            text = storageLine(state),
                            style = MaterialTheme.typography.labelSmall,
                            color = ParallaxTheme.colors.textTertiary,
                        )
                    }
                }
            },
            actions = {
                if (state.queuedCount > 0) {
                    ParallaxIconButton(
                        icon = ParallaxIcons.Queue,
                        contentDescription = "Processing queue, ${state.queuedCount} waiting",
                        onClick = onOpenQueue,
                    )
                }
                ParallaxIconButton(
                    icon = if (searching) ParallaxIcons.Close else ParallaxIcons.Search,
                    contentDescription = if (searching) "Close search" else "Search scans",
                    onClick = {
                        searching = !searching
                        if (!searching) onEvent(LibraryEvent.SearchChanged(""))
                    },
                )
                ParallaxIconButton(
                    icon = ParallaxIcons.ModeImport,
                    contentDescription = "Import a Parallax bundle",
                    onClick = onImport,
                )
                ParallaxIconButton(
                    icon = ParallaxIcons.Settings,
                    contentDescription = "Settings",
                    onClick = onOpenSettings,
                )
            },
            colors = TopAppBarDefaults.topAppBarColors(
                containerColor = ParallaxTheme.colors.surfaceSunken,
                titleContentColor = ParallaxTheme.colors.textPrimary,
            ),
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SelectionTopBar(state: LibraryUiState, onEvent: (LibraryEvent) -> Unit) {
    TopAppBar(
        title = { Text("${state.selection.size} selected") },
        navigationIcon = {
            ParallaxIconButton(
                icon = ParallaxIcons.Close,
                contentDescription = "Clear selection",
                onClick = { onEvent(LibraryEvent.ClearSelection) },
            )
        },
        actions = {
            ParallaxIconButton(
                icon = ParallaxIcons.Check,
                contentDescription = "Select all",
                onClick = { onEvent(LibraryEvent.SelectAll) },
            )
            ParallaxIconButton(
                icon = ParallaxIcons.Folder,
                contentDescription = "Move to a folder",
                onClick = { onEvent(LibraryEvent.RequestMove(state.selection)) },
            )
            ParallaxIconButton(
                icon = ParallaxIcons.Edit,
                contentDescription = "Rename",
                enabled = state.selection.size == 1,
                onClick = {
                    val single = state.projects.firstOrNull { it.id in state.selection }
                    if (single != null) onEvent(LibraryEvent.RequestRename(single))
                },
            )
            ParallaxIconButton(
                icon = ParallaxIcons.Delete,
                contentDescription = "Delete",
                onClick = { onEvent(LibraryEvent.RequestDelete(state.selection)) },
            )
        },
        colors = TopAppBarDefaults.topAppBarColors(
            containerColor = ParallaxTheme.colors.surfaceOverlay,
            titleContentColor = ParallaxTheme.colors.textPrimary,
        ),
    )
}

@Composable
private fun SearchField(value: String, onValueChange: (String) -> Unit, onDone: () -> Unit) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        modifier = Modifier.fillMaxWidth(),
        placeholder = { Text("Search scans and notes") },
        singleLine = true,
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
        keyboardActions = KeyboardActions(onSearch = { onDone() }),
    )
}

/** Folder, mode and favourite filters, plus the sort order. */
@Composable
private fun FilterRow(state: LibraryUiState, onEvent: (LibraryEvent) -> Unit) {
    LazyRow(
        contentPadding = PaddingValues(horizontal = Spacing.screenHorizontal),
        horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
        modifier = Modifier.fillMaxWidth().padding(vertical = Spacing.sm),
    ) {
        item {
            ParallaxChip(
                text = "All",
                selected = state.query.folderId == null && state.query.modeFilter == null &&
                    !state.query.favouritesOnly,
                onClick = { onEvent(LibraryEvent.ClearFilters) },
            )
        }
        item {
            ParallaxChip(
                text = "Favourites",
                selected = state.query.favouritesOnly,
                icon = ParallaxIcons.Favourite,
                onClick = { onEvent(LibraryEvent.ToggleFavouritesOnly) },
            )
        }
        items(items = CaptureMode.entries, key = { it.name }) { mode ->
            ParallaxChip(
                text = modeChipLabel(mode),
                selected = state.query.modeFilter == mode,
                onClick = {
                    onEvent(
                        LibraryEvent.ModeFilterChanged(
                            if (state.query.modeFilter == mode) null else mode,
                        ),
                    )
                },
            )
        }
        items(items = state.folders, key = { it.id }) { folder ->
            ParallaxChip(
                text = "${folder.name} (${folder.projectCount})",
                selected = state.query.folderId == folder.id,
                icon = ParallaxIcons.Folder,
                onClick = {
                    onEvent(
                        LibraryEvent.FolderSelected(
                            if (state.query.folderId == folder.id) null else folder.id,
                        ),
                    )
                },
            )
        }
        item {
            ParallaxChip(
                text = "New folder",
                selected = false,
                icon = ParallaxIcons.Add,
                onClick = { onEvent(LibraryEvent.RequestCreateFolder) },
            )
        }
        item {
            ParallaxChip(
                text = state.query.sort.label,
                selected = false,
                icon = ParallaxIcons.Sort,
                onClick = { onEvent(LibraryEvent.SortChanged(nextSort(state.query.sort))) },
            )
        }
    }
}

@Composable
private fun NewScanButton(onClick: () -> Unit) {
    ParallaxButton(
        text = "New scan",
        onClick = onClick,
        icon = ParallaxIcons.Scan,
        modifier = Modifier.padding(bottom = Spacing.lg),
    )
}

@Composable
private fun LibraryDialogs(state: LibraryUiState, onEvent: (LibraryEvent) -> Unit) {
    if (state.pendingDelete.isNotEmpty()) {
        val count = state.pendingDelete.size
        AlertDialog(
            onDismissRequest = { onEvent(LibraryEvent.CancelDelete) },
            title = { Text(if (count == 1) "Delete this scan?" else "Delete $count scans?") },
            text = {
                Text(
                    "This removes the model, the captured frames and every measurement. " +
                        "It cannot be undone, and nothing is backed up anywhere else.",
                )
            },
            confirmButton = {
                ParallaxTextButton(
                    text = "Delete",
                    onClick = { onEvent(LibraryEvent.ConfirmDelete) },
                    destructive = true,
                )
            },
            dismissButton = {
                ParallaxTextButton(
                    text = "Keep",
                    onClick = { onEvent(LibraryEvent.CancelDelete) },
                )
            },
            containerColor = ParallaxTheme.colors.surfaceRaised,
        )
    }

    state.renaming?.let { project ->
        TextPromptDialog(
            title = "Rename scan",
            initialValue = project.name,
            confirmLabel = "Rename",
            onConfirm = { onEvent(LibraryEvent.ConfirmRename(it)) },
            onDismiss = { onEvent(LibraryEvent.CancelRename) },
        )
    }

    if (state.creatingFolder) {
        TextPromptDialog(
            title = "New folder",
            initialValue = "",
            confirmLabel = "Create",
            onConfirm = { onEvent(LibraryEvent.ConfirmCreateFolder(it)) },
            onDismiss = { onEvent(LibraryEvent.CancelCreateFolder) },
        )
    }

    if (state.movingToFolder.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = { onEvent(LibraryEvent.CancelMove) },
            title = { Text("Move to…") },
            text = {
                Column {
                    ParallaxTextButton(
                        text = "No folder",
                        onClick = { onEvent(LibraryEvent.ConfirmMove(null)) },
                    )
                    state.folders.forEach { folder ->
                        ParallaxTextButton(
                            text = folder.name,
                            onClick = { onEvent(LibraryEvent.ConfirmMove(folder.id)) },
                        )
                    }
                }
            },
            confirmButton = {},
            dismissButton = {
                ParallaxTextButton(
                    text = "Cancel",
                    onClick = { onEvent(LibraryEvent.CancelMove) },
                )
            },
            containerColor = ParallaxTheme.colors.surfaceRaised,
        )
    }
}

@Composable
private fun TextPromptDialog(
    title: String,
    initialValue: String,
    confirmLabel: String,
    onConfirm: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var text by remember(initialValue) { mutableStateOf(initialValue) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
        },
        confirmButton = {
            ParallaxTextButton(
                text = confirmLabel,
                onClick = { onConfirm(text) },
                enabled = text.isNotBlank(),
            )
        },
        dismissButton = { ParallaxTextButton(text = "Cancel", onClick = onDismiss) },
        containerColor = ParallaxTheme.colors.surfaceRaised,
    )
}

private fun storageLine(state: LibraryUiState): String {
    val used = formatBytes(state.usedStorageBytes)
    val free = formatBytes(state.freeStorageBytes)
    return "${state.projects.size} scans · $used used · $free free"
}

private fun modeChipLabel(mode: CaptureMode) = when (mode) {
    CaptureMode.Object -> "Objects"
    CaptureMode.Room -> "Rooms"
    CaptureMode.Area -> "Areas"
    CaptureMode.Import -> "Imported"
}

/** Cycling the sort from one chip, rather than opening a menu for six options. */
private fun nextSort(current: LibrarySort): LibrarySort =
    LibrarySort.entries[(current.ordinal + 1) % LibrarySort.entries.size]

private const val MIN_CARD_WIDTH = 164
private val BUNDLE_MIME_TYPES = arrayOf("application/zip", "application/octet-stream")
