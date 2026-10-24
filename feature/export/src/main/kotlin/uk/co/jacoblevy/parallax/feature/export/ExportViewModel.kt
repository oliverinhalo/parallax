package uk.co.jacoblevy.parallax.feature.export

import android.net.Uri
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.data.storage.ProjectTransfer
import uk.co.jacoblevy.parallax.core.model.ExportFormat
import uk.co.jacoblevy.parallax.core.model.LengthUnit
import uk.co.jacoblevy.parallax.core.model.Project
import java.io.File
import javax.inject.Inject

/** Where a finished file should go. */
enum class ExportDestination(val label: String) {
    Share("Share"),
    SaveToFiles("Save to files"),
}

data class ExportUiState(
    val loading: Boolean = true,
    val project: Project? = null,
    val formats: List<ExportFormat> = emptyList(),
    val unavailable: List<ExportFormat> = emptyList(),
    val settings: ExportSettings = ExportSettings(),
    val destination: ExportDestination = ExportDestination.Share,
    val includeRawFramesInBundle: Boolean = false,
    val busyWith: ExportFormat? = null,
    val lastResult: String? = null,
    val error: String? = null,
)

/** One-shot things the screen has to do with the system, not state. */
sealed interface ExportEvent {
    /** Hand this file to the share sheet. */
    data class Share(val file: File, val mimeType: String) : ExportEvent

    /** Ask the system where to put a file of this name and type. */
    data class ChooseDestination(
        val suggestedName: String,
        val mimeType: String,
        val format: ExportFormat,
    ) : ExportEvent
}

/**
 * The export screen's state.
 *
 * Formats the scan cannot produce are listed separately rather than hidden: a
 * user looking for USDZ on a point-cloud-only scan needs to know it is missing
 * because there is no mesh, not be left wondering whether the app has it.
 */
@HiltViewModel
class ExportViewModel @Inject constructor(
    savedState: SavedStateHandle,
    private val projects: ProjectRepository,
    private val settings: SettingsRepository,
    private val storage: ProjectStorage,
    private val transfer: ProjectTransfer,
    private val exporter: Exporter,
) : ViewModel() {

    private val projectId: String = savedState["projectId"] ?: ""
    private val _state = MutableStateFlow(ExportUiState())
    val state: StateFlow<ExportUiState> = _state.asStateFlow()

    private val _events = MutableSharedFlow<ExportEvent>(
        replay = 0,
        extraBufferCapacity = 4,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    val events: Flow<ExportEvent> = _events.asSharedFlow()

    /** Set while a SAF destination is being chosen, so the write can resume. */
    private var pendingFormat: ExportFormat? = null

    init {
        viewModelScope.launch {
            val project = projects.get(projectId)
            if (project == null) {
                _state.value = _state.value.copy(
                    loading = false,
                    error = "This scan is no longer here.",
                )
                return@launch
            }
            val available = ExportFormat.entries.filter { it.isAvailableFor(project.outputs) }
            _state.value = _state.value.copy(
                loading = false,
                project = project,
                formats = available,
                unavailable = ExportFormat.entries - available.toSet(),
                settings = _state.value.settings.copy(
                    unit = settings.settings.first().units,
                ),
            )
        }
    }

    fun setUnit(unit: LengthUnit) {
        _state.value = _state.value.copy(settings = _state.value.settings.copy(unit = unit))
    }

    fun setZUp(zUp: Boolean) {
        _state.value = _state.value.copy(settings = _state.value.settings.copy(zUp = zUp))
    }

    fun setDarkFloorPlan(dark: Boolean) {
        _state.value = _state.value.copy(
            settings = _state.value.settings.copy(darkFloorPlan = dark),
        )
    }

    fun setDestination(destination: ExportDestination) {
        _state.value = _state.value.copy(destination = destination)
    }

    fun setIncludeRawFrames(include: Boolean) {
        _state.value = _state.value.copy(includeRawFramesInBundle = include)
    }

    fun dismissMessage() {
        _state.value = _state.value.copy(lastResult = null, error = null)
    }

    /**
     * Produce [format].
     *
     * Sharing writes into the staging directory the FileProvider can read, and
     * saving to files writes into the project and then copies into whatever the
     * user picks -- so a cancelled save dialog leaves no half-written file
     * anywhere the user can see.
     */
    fun export(format: ExportFormat) {
        val project = _state.value.project ?: return
        if (_state.value.busyWith != null) return
        _state.value = _state.value.copy(busyWith = format, lastResult = null, error = null)

        viewModelScope.launch {
            if (_state.value.destination == ExportDestination.SaveToFiles) {
                pendingFormat = format
                _state.value = _state.value.copy(busyWith = null)
                _events.emit(
                    ExportEvent.ChooseDestination(
                        suggestedName = exporter.suggestedName(project, format),
                        mimeType = format.mimeType,
                        format = format,
                    ),
                )
                return@launch
            }
            produce(project, format)
                .onSuccess { file ->
                    val staged = stageForSharing(file)
                    _state.value = _state.value.copy(busyWith = null)
                    _events.emit(ExportEvent.Share(staged, format.mimeType))
                }
                .onFailure { failure -> report(failure) }
        }
    }

    /** Called back with whatever the system file picker returned. */
    fun writeTo(uri: Uri?) {
        val format = pendingFormat ?: return
        pendingFormat = null
        val project = _state.value.project ?: return
        if (uri == null) return  // the user cancelled, which is not an error

        _state.value = _state.value.copy(busyWith = format)
        viewModelScope.launch {
            runCatching {
                if (format == ExportFormat.ProjectBundle) {
                    transfer.export(project.id, uri, _state.value.includeRawFramesInBundle)
                    "Bundle saved"
                } else {
                    val file = produce(project, format).getOrThrow()
                    transfer.copyToUri(file, uri)
                    "${format.label} saved"
                }
            }
                .onSuccess { message ->
                    _state.value = _state.value.copy(busyWith = null, lastResult = message)
                }
                .onFailure { failure -> report(failure) }
        }
    }

    private suspend fun produce(project: Project, format: ExportFormat): Result<File> =
        if (format == ExportFormat.ProjectBundle) {
            // The bundle writer streams to a destination, so sharing one means
            // staging it first and sharing the staged copy.
            runCatching {
                val staged = transfer.stagingFile(transfer.suggestedBundleName(project))
                staged.outputStream().use { stream ->
                    projects.exportBundle(
                        project.id,
                        stream,
                        _state.value.includeRawFramesInBundle,
                    )
                }
                staged
            }
        } else {
            exporter.export(project, format, _state.value.settings).map { it.file }
        }

    /**
     * Copy into the one directory the FileProvider exposes.
     *
     * Sharing a file from inside the project directory would mean exposing the
     * whole of it -- every frame, every checkpoint -- to any app that receives
     * the URI. Staging a copy keeps the exposed surface to the file that was
     * actually shared.
     */
    private fun stageForSharing(file: File): File {
        if (file.parentFile == storage.sharedDir()) return file
        val staged = storage.sharedFile(file.name)
        file.copyTo(staged, overwrite = true)
        return staged
    }

    private fun report(failure: Throwable) {
        _state.value = _state.value.copy(
            busyWith = null,
            error = failure.message ?: "That export did not work.",
        )
    }
}
