package uk.co.jacoblevy.parallax.feature.library

import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.toParallaxError
import uk.co.jacoblevy.parallax.core.common.util.formatBytes
import uk.co.jacoblevy.parallax.core.data.repository.FolderRepository
import uk.co.jacoblevy.parallax.core.data.repository.IdGenerator
import uk.co.jacoblevy.parallax.core.data.repository.ProcessingRepository
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.data.storage.ProjectTransfer
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.Folder
import uk.co.jacoblevy.parallax.core.model.LibraryQuery
import uk.co.jacoblevy.parallax.core.model.LibrarySort
import uk.co.jacoblevy.parallax.core.model.Project
import java.io.File
import javax.inject.Inject

/** Everything the library screen draws. */
data class LibraryUiState(
    val loading: Boolean = true,
    val projects: List<Project> = emptyList(),
    val folders: List<Folder> = emptyList(),
    val query: LibraryQuery = LibraryQuery(),
    val usedStorageBytes: Long = 0,
    val freeStorageBytes: Long = 0,
    val queuedCount: Int = 0,
    /** Multi-select, entered by long-pressing a card. */
    val selection: Set<String> = emptySet(),
    val pendingDelete: Set<String> = emptySet(),
    val renaming: Project? = null,
    val movingToFolder: Set<String> = emptySet(),
    val creatingFolder: Boolean = false,
    val importing: Boolean = false,
    val error: ParallaxError? = null,
    val notice: String? = null,
) {
    val inSelectionMode: Boolean get() = selection.isNotEmpty()

    /** True when the library is genuinely empty, rather than filtered down to nothing. */
    val isEmpty: Boolean get() = !loading && projects.isEmpty() && !query.isFiltered

    /** True when a filter hid everything, which needs a different message and a reset action. */
    val isFilteredEmpty: Boolean get() = !loading && projects.isEmpty() && query.isFiltered
}

/**
 * Everything the library screen can ask for.
 *
 * Grouped into sub-interfaces so the ViewModel dispatches six small handlers rather than one
 * thirty-branch `when`. The concrete names are unchanged, so call sites still read
 * `LibraryEvent.SearchChanged(...)`.
 */
sealed interface LibraryEvent {

    /** Changes to what the grid shows. These are pure state, never a repository call. */
    sealed interface Filtering : LibraryEvent

    /** Multi-select, entered by long-pressing a card. */
    sealed interface Selecting : LibraryEvent

    /** Deleting scans, which is confirmed before it happens. */
    sealed interface Deleting : LibraryEvent

    /** Renaming a single scan. */
    sealed interface Renaming : LibraryEvent

    /** Folders, and moving scans between them. */
    sealed interface Organising : LibraryEvent

    /** Transient messages the screen has shown and is done with. */
    sealed interface Dismissing : LibraryEvent

    data class SearchChanged(val text: String) : Filtering
    data class SortChanged(val sort: LibrarySort) : Filtering
    data class FolderSelected(val folderId: String?) : Filtering
    data class ModeFilterChanged(val mode: CaptureMode?) : Filtering
    data object ToggleFavouritesOnly : Filtering
    data object ClearFilters : Filtering

    data class ToggleFavourite(val projectId: String) : Selecting
    data class ToggleSelected(val projectId: String) : Selecting
    data object ClearSelection : Selecting
    data object SelectAll : Selecting

    data class RequestDelete(val projectIds: Set<String>) : Deleting
    data object ConfirmDelete : Deleting
    data object CancelDelete : Deleting

    data class RequestRename(val project: Project) : Renaming
    data class ConfirmRename(val name: String) : Renaming
    data object CancelRename : Renaming

    data class RequestMove(val projectIds: Set<String>) : Organising
    data class ConfirmMove(val folderId: String?) : Organising
    data object CancelMove : Organising
    data object RequestCreateFolder : Organising
    data class ConfirmCreateFolder(val name: String) : Organising
    data object CancelCreateFolder : Organising
    data class DeleteFolder(val folderId: String) : Organising

    /** A `.parallax` bundle the user picked through the Storage Access Framework. */
    data class ImportBundle(val uri: Uri) : LibraryEvent

    data object DismissError : Dismissing
    data object DismissNotice : Dismissing
}

@OptIn(FlowPreview::class)
@HiltViewModel
class LibraryViewModel @Inject constructor(
    private val projects: ProjectRepository,
    private val folders: FolderRepository,
    private val processing: ProcessingRepository,
    private val storage: ProjectStorage,
    private val transfer: ProjectTransfer,
    private val ids: IdGenerator,
) : ViewModel() {

    private val query = MutableStateFlow(LibraryQuery())
    private val local = MutableStateFlow(LibraryUiState())

    val state: StateFlow<LibraryUiState> = local.asStateFlow()

    init {
        // Search is debounced but sort and filters are not: a filter chip should respond on the
        // frame it is tapped, while re-querying on every keystroke makes typing feel sticky.
        // `debounce` is still marked FlowPreview after years of stability; the per-value variant
        // is what lets a blank search pass through with no delay at all, so the first paint is
        // not held up by a timeout that exists only for typing.
        val debouncedQuery = query
            .debounce { if (it.search.isBlank()) 0L else SEARCH_DEBOUNCE_MILLIS }
            .distinctUntilChanged()

        combine(
            projects.observeLibrary(debouncedQuery),
            folders.observeAll(),
            processing.observePendingCount(),
            projects.observeTotalStorageBytes(),
            query,
        ) { visible, folderList, queued, used, currentQuery ->
            Snapshot(visible, folderList, queued, used, currentQuery)
        }
            .onEach { snapshot ->
                local.update { previous ->
                    previous.copy(
                        loading = false,
                        projects = snapshot.projects,
                        folders = snapshot.folders,
                        query = snapshot.query,
                        queuedCount = snapshot.queued,
                        usedStorageBytes = snapshot.usedBytes,
                        freeStorageBytes = storage.deviceFreeBytes(),
                        // A project deleted from under a selection must leave it, or the action
                        // bar would offer to delete something that is already gone.
                        selection = previous.selection.intersect(
                            snapshot.projects.map { it.id }.toSet(),
                        ),
                    )
                }
            }
            .launchIn(viewModelScope)

        // Reconcile once: a crash between creating a directory and committing its row would
        // otherwise strand the frames of an abandoned capture forever.
        viewModelScope.launch { runCatching { projects.reconcile() } }
    }

    private data class Snapshot(
        val projects: List<Project>,
        val folders: List<Folder>,
        val queued: Int,
        val usedBytes: Long,
        val query: LibraryQuery,
    )

    fun onEvent(event: LibraryEvent) {
        when (event) {
            is LibraryEvent.Filtering -> onFiltering(event)
            is LibraryEvent.Selecting -> onSelecting(event)
            is LibraryEvent.Deleting -> onDeleting(event)
            is LibraryEvent.Renaming -> onRenaming(event)
            is LibraryEvent.Organising -> onOrganising(event)
            is LibraryEvent.ImportBundle -> onImport(event.uri)
            is LibraryEvent.Dismissing -> onDismissing(event)
        }
    }

    private fun onFiltering(event: LibraryEvent.Filtering) {
        when (event) {
            is LibraryEvent.SearchChanged -> query.update { it.copy(search = event.text) }
            is LibraryEvent.SortChanged -> query.update { it.copy(sort = event.sort) }
            is LibraryEvent.FolderSelected -> query.update { it.copy(folderId = event.folderId) }
            is LibraryEvent.ModeFilterChanged -> query.update { it.copy(modeFilter = event.mode) }
            LibraryEvent.ToggleFavouritesOnly ->
                query.update { it.copy(favouritesOnly = !it.favouritesOnly) }
            // The sort survives a filter reset: it is the user's reading preference, not part
            // of what they were narrowing down to.
            LibraryEvent.ClearFilters -> query.value = LibraryQuery(sort = query.value.sort)
        }
    }

    private fun onSelecting(event: LibraryEvent.Selecting) {
        when (event) {
            is LibraryEvent.ToggleFavourite -> {
                val project = local.value.projects.firstOrNull { it.id == event.projectId }
                if (project != null) {
                    guard { projects.setFavourite(project.id, !project.favourite) }
                }
            }
            is LibraryEvent.ToggleSelected -> local.update { current ->
                val selection = current.selection.toMutableSet()
                if (!selection.add(event.projectId)) selection.remove(event.projectId)
                current.copy(selection = selection)
            }
            LibraryEvent.ClearSelection -> local.update { it.copy(selection = emptySet()) }
            LibraryEvent.SelectAll ->
                local.update { it.copy(selection = it.projects.map { p -> p.id }.toSet()) }
        }
    }

    private fun onDeleting(event: LibraryEvent.Deleting) {
        when (event) {
            is LibraryEvent.RequestDelete ->
                local.update { it.copy(pendingDelete = event.projectIds) }
            LibraryEvent.CancelDelete -> local.update { it.copy(pendingDelete = emptySet()) }
            LibraryEvent.ConfirmDelete -> {
                val targets = local.value.pendingDelete
                local.update { it.copy(pendingDelete = emptySet(), selection = emptySet()) }
                guard {
                    // Size is read before the delete, so the confirmation can say how much the
                    // user actually got back.
                    var freed = 0L
                    for (id in targets) {
                        freed += storage.sizeOf(id)
                        projects.delete(id)
                    }
                    local.update { it.copy(notice = deletionNotice(targets.size, freed)) }
                }
            }
        }
    }

    private fun onRenaming(event: LibraryEvent.Renaming) {
        when (event) {
            is LibraryEvent.RequestRename -> local.update { it.copy(renaming = event.project) }
            LibraryEvent.CancelRename -> local.update { it.copy(renaming = null) }
            is LibraryEvent.ConfirmRename -> {
                val target = local.value.renaming
                local.update { it.copy(renaming = null) }
                if (target != null) guard { projects.rename(target.id, event.name) }
            }
        }
    }

    private fun onOrganising(event: LibraryEvent.Organising) {
        when (event) {
            is LibraryEvent.RequestMove ->
                local.update { it.copy(movingToFolder = event.projectIds) }
            LibraryEvent.CancelMove -> local.update { it.copy(movingToFolder = emptySet()) }
            is LibraryEvent.ConfirmMove -> {
                val targets = local.value.movingToFolder
                local.update { it.copy(movingToFolder = emptySet(), selection = emptySet()) }
                guard { for (id in targets) projects.moveToFolder(id, event.folderId) }
            }
            LibraryEvent.RequestCreateFolder -> local.update { it.copy(creatingFolder = true) }
            LibraryEvent.CancelCreateFolder -> local.update { it.copy(creatingFolder = false) }
            is LibraryEvent.ConfirmCreateFolder -> {
                local.update { it.copy(creatingFolder = false) }
                guard { folders.create(ids.newId(), event.name) }
            }
            is LibraryEvent.DeleteFolder -> guard {
                folders.delete(event.folderId)
                // The folder's projects survive as unfiled, so a filter pointing at the deleted
                // folder has to be cleared or the library would look empty, which reads as
                // data loss.
                if (query.value.folderId == event.folderId) {
                    query.update { it.copy(folderId = null) }
                }
            }
        }
    }

    private fun onImport(uri: Uri) {
        local.update { it.copy(importing = true) }
        guard {
            val imported = transfer.import(uri)
            local.update {
                it.copy(importing = false, notice = "Imported \"${imported.name}\"")
            }
        }
    }

    private fun onDismissing(event: LibraryEvent.Dismissing) {
        when (event) {
            LibraryEvent.DismissError -> local.update { it.copy(error = null) }
            LibraryEvent.DismissNotice -> local.update { it.copy(notice = null) }
        }
    }

    /**
     * The thumbnail file for a project, or null when none has been written yet.
     *
     * A path rather than a decoded bitmap: the grid decodes lazily and at a bounded size, so a
     * library of fifty scans never holds fifty full-resolution images.
     */
    fun thumbnailFor(project: Project): File? =
        storage.thumbnailFile(project.id).takeIf { it.isFile }

    /**
     * Run a repository action, turning any failure into an error the screen can show.
     *
     * Every mutation goes through this. A library action that fails silently -- a delete that
     * did not happen, a rename that did not stick -- is worse than one that reports why.
     */
    private fun guard(block: suspend () -> Unit) {
        viewModelScope.launch {
            runCatching { block() }
                .onFailure { failure ->
                    local.update { it.copy(importing = false, error = failure.toParallaxError()) }
                }
        }
    }

    private fun deletionNotice(count: Int, freedBytes: Long): String {
        val subject = if (count == 1) "Scan deleted" else "$count scans deleted"
        return "$subject · ${formatBytes(freedBytes)} freed"
    }

    private companion object {
        const val SEARCH_DEBOUNCE_MILLIS = 180L
    }
}

/** The folder a project sits in, for the card's subtitle. */
internal fun LibraryUiState.folderNameFor(project: Project): String? =
    project.folderId?.let { id -> folders.firstOrNull { it.id == id }?.name }
