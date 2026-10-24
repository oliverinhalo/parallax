package uk.co.jacoblevy.parallax.feature.ar

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import uk.co.jacoblevy.parallax.core.common.di.IoDispatcher
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.BoundingBox
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.engine.nativebridge.ParallaxEngine
import uk.co.jacoblevy.parallax.feature.ar.logic.Placement
import javax.inject.Inject

/** The model's geometry, ready for the GL thread. */
data class ArModelGeometry(
    val vertices: FloatArray,
    val indices: IntArray,
    val bounds: BoundingBox,
) {
    override fun equals(other: Any?): Boolean =
        other is ArModelGeometry && vertices.size == other.vertices.size &&
            indices.size == other.indices.size && bounds == other.bounds

    override fun hashCode(): Int = 31 * (31 * vertices.size + indices.size) + bounds.hashCode()
}

data class ArPlacementUiState(
    val loading: Boolean = true,
    val project: Project? = null,
    val geometry: ArModelGeometry? = null,
    val placement: Placement = Placement(),
    val trackedSurfaces: Int = 0,
    val imperialUnits: Boolean = false,
    val transparency: Float = 0f,
    val error: String? = null,
) {
    /** What the screen should be telling the user to do. */
    val guidance: String
        get() = when {
            error != null -> error
            geometry == null -> "Getting the scan ready…"
            placement.isPlaced -> "Drag to move, pinch to resize, twist to turn."
            trackedSurfaces > 0 -> "Tap the floor to put it down."
            else -> "Move the phone slowly to find the floor."
        }
}

/**
 * Placing a finished scan in the room you are standing in.
 *
 * The geometry is loaded once and decimated hard: this screen draws the model
 * over a live camera feed at sixty frames a second on a phone that is also
 * running ARCore's tracking, and a three-hundred-thousand-triangle scan would
 * halve the frame rate for detail nobody can see at this size.
 */
@HiltViewModel
class ArPlacementViewModel @Inject constructor(
    savedState: SavedStateHandle,
    private val projects: ProjectRepository,
    private val settings: SettingsRepository,
    private val storage: ProjectStorage,
    @IoDispatcher private val io: CoroutineDispatcher,
) : ViewModel() {

    private val projectId: String = savedState["projectId"] ?: ""
    private val _state = MutableStateFlow(ArPlacementUiState())
    val state: StateFlow<ArPlacementUiState> = _state.asStateFlow()

    init {
        viewModelScope.launch { load() }
    }

    private suspend fun load() {
        val project = projects.get(projectId)
        if (project == null) {
            _state.value = _state.value.copy(
                loading = false,
                error = "This scan is no longer here.",
            )
            return
        }
        val imperial = settings.settings.first().imperial
        _state.value = _state.value.copy(
            loading = false,
            project = project,
            imperialUnits = imperial,
        )
        val geometry = withContext(io) { loadGeometry() }
        if (geometry == null) {
            _state.value = _state.value.copy(error = "This scan has no model to place.")
            return
        }
        _state.value = _state.value.copy(
            geometry = geometry,
            placement = _state.value.placement.copy(bounds = geometry.bounds),
        )
    }

    private fun loadGeometry(): ArModelGeometry? {
        val handle = runCatching {
            ParallaxEngine.createPipeline(
                """{"projectDir":"${storage.projectDir(projectId).absolutePath}"}""",
            )
        }.getOrDefault(0L)
        if (handle == 0L) return null
        return try {
            val vertices = ParallaxEngine.loadMeshVertices(handle) ?: return null
            val indices = ParallaxEngine.loadMeshIndices(handle) ?: return null
            if (vertices.isEmpty() || indices.isEmpty()) return null
            ArModelGeometry(vertices, indices, boundsOf(vertices))
        } finally {
            ParallaxEngine.destroyPipeline(handle)
        }
    }

    private fun boundsOf(vertices: FloatArray): BoundingBox {
        var minX = Float.MAX_VALUE
        var minY = Float.MAX_VALUE
        var minZ = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE
        var maxY = -Float.MAX_VALUE
        var maxZ = -Float.MAX_VALUE
        var i = 0
        while (i + FLOATS_PER_VERTEX - 1 < vertices.size) {
            minX = minOf(minX, vertices[i]); maxX = maxOf(maxX, vertices[i])
            minY = minOf(minY, vertices[i + 1]); maxY = maxOf(maxY, vertices[i + 1])
            minZ = minOf(minZ, vertices[i + 2]); maxZ = maxOf(maxZ, vertices[i + 2])
            i += FLOATS_PER_VERTEX
        }
        if (minX > maxX) return BoundingBox()
        return BoundingBox(minX, minY, minZ, maxX, maxY, maxZ)
    }

    // ------------------------------------------------------------- tracking

    fun surfacesTracked(count: Int) {
        val current = _state.value
        if (count == current.trackedSurfaces) return
        val placement = if (count > 0) {
            current.placement.surfaceFound()
        } else {
            current.placement.surfaceLost()
        }
        _state.value = current.copy(trackedSurfaces = count, placement = placement)
    }

    fun sessionFailed(message: String) {
        _state.value = _state.value.copy(error = message)
    }

    // ---------------------------------------------------------- interaction

    /** A tap that the session resolved to a point on a surface. */
    fun placeAt(x: Float, y: Float, z: Float) {
        val geometry = _state.value.geometry ?: return
        _state.value = _state.value.copy(
            placement = _state.value.placement.placeAt(x, y, z, geometry.bounds),
        )
    }

    fun rotate(radians: Float) {
        _state.value = _state.value.copy(placement = _state.value.placement.rotatedBy(radians))
    }

    fun scale(factor: Float) {
        _state.value = _state.value.copy(placement = _state.value.placement.scaledBy(factor))
    }

    fun move(deltaX: Float, deltaZ: Float) {
        _state.value = _state.value.copy(placement = _state.value.placement.movedBy(deltaX, deltaZ))
    }

    fun snapToRightAngle() {
        _state.value = _state.value.copy(
            placement = _state.value.placement.snappedToRightAngle(),
        )
    }

    fun setLocked(locked: Boolean) {
        _state.value = _state.value.copy(placement = _state.value.placement.withLock(locked))
    }

    fun setTransparency(value: Float) {
        _state.value = _state.value.copy(transparency = value.coerceIn(0f, MAX_TRANSPARENCY))
    }

    fun reset() {
        _state.value = _state.value.copy(placement = _state.value.placement.reset())
    }

    fun dismissError() {
        _state.value = _state.value.copy(error = null)
    }

    private companion object {
        /**
         * How transparent the model may be made.
         *
         * Being able to see through it is how you check it lines up with the
         * real room; being able to make it invisible is just confusing.
         */
        const val MAX_TRANSPARENCY = 0.7f

        /** The engine hands vertices over as a flat x, y, z array. */
        const val FLOATS_PER_VERTEX = 3
    }
}
