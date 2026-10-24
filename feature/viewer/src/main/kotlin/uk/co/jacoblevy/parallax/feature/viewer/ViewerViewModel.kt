package uk.co.jacoblevy.parallax.feature.viewer

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.data.repository.IdGenerator
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.model.Annotation
import uk.co.jacoblevy.parallax.core.model.BoundingBox
import uk.co.jacoblevy.parallax.core.model.LightingPreset
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.ModelEdit
import uk.co.jacoblevy.parallax.core.model.Point3
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.RenderMode
import uk.co.jacoblevy.parallax.feature.viewer.camera.OrbitCamera
import uk.co.jacoblevy.parallax.feature.viewer.camera.WalkCamera
import uk.co.jacoblevy.parallax.feature.viewer.edit.EditPlan
import uk.co.jacoblevy.parallax.feature.viewer.edit.planFor
import uk.co.jacoblevy.parallax.feature.viewer.measure.MeasureDraft
import uk.co.jacoblevy.parallax.feature.viewer.measure.MeasureTool
import uk.co.jacoblevy.parallax.feature.viewer.pick.TrianglePicker
import uk.co.jacoblevy.parallax.feature.viewer.pick.screenRay
import uk.co.jacoblevy.parallax.feature.viewer.render.ModelLoader
import java.io.File
import javax.inject.Inject

/** What the user is doing with taps on the model. */
enum class ViewerInteraction { Look, Measure, Annotate, Crop }

data class ViewerUiState(
    val loading: Boolean = true,
    val project: Project? = null,
    val availableModes: List<RenderMode> = emptyList(),
    val renderMode: RenderMode = RenderMode.Shaded,
    val lighting: LightingPreset = LightingPreset.Studio,
    val exposureStops: Float = 0f,
    val walking: Boolean = false,
    val orbit: OrbitCamera = OrbitCamera(),
    val walk: WalkCamera = WalkCamera(0f, 0f, 0f),
    val interaction: ViewerInteraction = ViewerInteraction.Look,
    val measureTool: MeasureTool = MeasureTool.Distance,
    val draft: MeasureDraft? = null,
    val measurements: List<Measurement> = emptyList(),
    val annotations: List<Annotation> = emptyList(),
    val pendingAnnotation: Point3? = null,
    val pendingAnnotationNormal: Point3? = null,
    val edits: List<ModelEdit> = emptyList(),
    val imperialUnits: Boolean = false,
    val modelFile: File? = null,
    val rendererFailed: Boolean = false,
    val message: String? = null,
) {
    val editPlan: EditPlan get() = planFor(edits)
    val canUndoEdit: Boolean get() = edits.isNotEmpty()

    /** The bounds the camera was framed against, after any edits. */
    val bounds: BoundingBox
        get() = project?.measurements?.boundsMetres ?: BoundingBox()
}

/**
 * The viewer's state, kept here rather than in the composition.
 *
 * All of it: the camera, the half-finished measurement, which pin is being
 * written. A rotation or a process-death restore must not throw away a
 * measurement someone is three taps into, and the only way to be sure of that
 * is for the composition to own none of it.
 */
@HiltViewModel
class ViewerViewModel @Inject constructor(
    savedState: SavedStateHandle,
    private val projects: ProjectRepository,
    private val settings: SettingsRepository,
    private val loader: ModelLoader,
    private val ids: IdGenerator,
) : ViewModel() {

    private val projectId: String = savedState["projectId"] ?: ""
    private val _state = MutableStateFlow(ViewerUiState())
    val state: StateFlow<ViewerUiState> = _state.asStateFlow()

    private var picker: TrianglePicker? = null

    init {
        viewModelScope.launch { observeProject() }
        viewModelScope.launch {
            projects.observeMeasurements(projectId).collect { saved ->
                _state.value = _state.value.copy(measurements = saved)
            }
        }
        viewModelScope.launch {
            projects.observeAnnotations(projectId).collect { saved ->
                _state.value = _state.value.copy(annotations = saved)
            }
        }
        viewModelScope.launch {
            val imperial = settings.settings.first().imperial
            _state.value = _state.value.copy(imperialUnits = imperial)
        }
    }

    private suspend fun observeProject() {
        val project = projects.get(projectId)
        if (project == null) {
            _state.value = _state.value.copy(
                loading = false,
                message = "This scan is no longer here.",
            )
            return
        }
        val modes = RenderMode.entries.filter { it.isAvailableFor(project.outputs) }
        val mode = modes.firstOrNull() ?: RenderMode.Shaded
        val bounds = project.measurements?.boundsMetres ?: BoundingBox()
        _state.value = _state.value.copy(
            loading = false,
            project = project,
            availableModes = modes,
            renderMode = mode,
            orbit = OrbitCamera.framing(bounds),
            walk = WalkCamera.standingIn(bounds),
            edits = project.edits,
        )
        loadModel(mode)
    }

    // ------------------------------------------------------------- rendering

    fun setRenderMode(mode: RenderMode) {
        if (mode == _state.value.renderMode) return
        _state.value = _state.value.copy(renderMode = mode)
        loadModel(mode)
    }

    private fun loadModel(mode: RenderMode) = viewModelScope.launch {
        loader.load(projectId, mode)
            .onSuccess { loaded ->
                picker = loaded.picker
                _state.value = _state.value.copy(modelFile = loaded.glb, message = null)
            }
            .onFailure { failure ->
                _state.value = _state.value.copy(
                    modelFile = null,
                    message = failure.message ?: "That view could not be prepared.",
                )
            }
    }

    fun setLighting(preset: LightingPreset) {
        _state.value = _state.value.copy(lighting = preset)
    }

    fun setExposure(stops: Float) {
        _state.value = _state.value.copy(
            exposureStops = stops.coerceIn(-EXPOSURE_LIMIT_STOPS, EXPOSURE_LIMIT_STOPS),
        )
    }

    fun rendererFailed() {
        _state.value = _state.value.copy(rendererFailed = true)
    }

    fun dismissMessage() {
        _state.value = _state.value.copy(message = null)
    }

    // --------------------------------------------------------------- camera

    fun setWalking(walking: Boolean) {
        val bounds = _state.value.bounds
        _state.value = _state.value.copy(
            walking = walking,
            // Re-entering walk mode from the middle of the room, rather than
            // wherever the last walk ended, which is usually in a wall.
            walk = if (walking) WalkCamera.standingIn(bounds) else _state.value.walk,
            interaction = if (walking) ViewerInteraction.Look else _state.value.interaction,
        )
    }

    fun rotate(deltaYaw: Float, deltaPitch: Float) {
        val current = _state.value
        if (current.walking) {
            _state.value = current.copy(walk = current.walk.looked(deltaYaw, deltaPitch))
        } else {
            _state.value = current.copy(orbit = current.orbit.stopped().rotatedBy(deltaYaw, deltaPitch))
        }
    }

    fun pan(deltaRight: Float, deltaUp: Float) {
        val current = _state.value
        if (current.walking) return
        _state.value = current.copy(orbit = current.orbit.pannedBy(deltaRight, deltaUp))
    }

    fun zoom(scale: Float) {
        val current = _state.value
        if (current.walking) return
        _state.value = current.copy(orbit = current.orbit.zoomedBy(scale))
    }

    fun fling(yawPerSecond: Float, pitchPerSecond: Float) {
        val current = _state.value
        if (current.walking) return
        _state.value = current.copy(orbit = current.orbit.withFling(yawPerSecond, pitchPerSecond))
    }

    /** Advance inertia. Called once a frame while the camera is still moving. */
    fun advanceCamera(seconds: Float) {
        val current = _state.value
        if (current.walking || current.orbit.isSettled) return
        _state.value = current.copy(orbit = current.orbit.advanced(seconds))
    }

    fun walk(forwardMetres: Float, strafeMetres: Float) {
        val current = _state.value
        if (!current.walking) return
        _state.value = current.copy(walk = current.walk.moved(forwardMetres, strafeMetres))
    }

    fun setEyeHeight(metres: Float) {
        val current = _state.value
        _state.value = current.copy(walk = current.walk.withEyeHeight(metres))
    }

    fun resetView() {
        val bounds = _state.value.bounds
        _state.value = _state.value.copy(
            orbit = OrbitCamera.framing(bounds),
            walk = WalkCamera.standingIn(bounds),
        )
    }

    // ----------------------------------------------------------- interaction

    fun setInteraction(interaction: ViewerInteraction) {
        _state.value = _state.value.copy(
            interaction = interaction,
            draft = if (interaction == ViewerInteraction.Measure) {
                MeasureDraft(_state.value.measureTool)
            } else {
                null
            },
            pendingAnnotation = null,
        )
    }

    fun setMeasureTool(tool: MeasureTool) {
        _state.value = _state.value.copy(measureTool = tool, draft = MeasureDraft(tool))
    }

    /**
     * A tap on the model.
     *
     * Returns false when nothing was under the finger, which the screen uses
     * to say so: a measurement that silently does not appear reads as a bug,
     * and "tap on the model" is a fixable mistake.
     */
    fun tap(screenX: Float, screenY: Float, viewportWidth: Int, viewportHeight: Int): Boolean {
        val current = _state.value
        if (current.interaction == ViewerInteraction.Look || current.walking) return false
        val currentPicker = picker ?: return false
        val ray = screenRay(current.orbit, viewportWidth, viewportHeight, screenX, screenY)
        val hit = currentPicker.intersect(ray) ?: return false

        when (current.interaction) {
            ViewerInteraction.Measure -> {
                val draft = (current.draft ?: MeasureDraft(current.measureTool)).withPoint(hit.point)
                _state.value = current.copy(draft = draft)
                if (draft.isComplete) saveDraft()
            }
            ViewerInteraction.Annotate -> {
                _state.value = current.copy(
                    pendingAnnotation = hit.point,
                    pendingAnnotationNormal = hit.normal,
                )
            }
            else -> return false
        }
        return true
    }

    fun undoMeasurePoint() {
        val draft = _state.value.draft ?: return
        _state.value = _state.value.copy(draft = draft.withoutLastPoint())
    }

    fun saveDraft() {
        val current = _state.value
        val draft = current.draft ?: return
        val measurement = draft.build(ids.newId(), labelFor(draft.tool, current.measurements))
            ?: return
        viewModelScope.launch {
            projects.upsertMeasurement(projectId, measurement)
            _state.value = _state.value.copy(draft = MeasureDraft(current.measureTool))
        }
    }

    fun deleteMeasurement(id: String) = viewModelScope.launch {
        projects.deleteMeasurement(id)
    }

    fun saveAnnotation(title: String, body: String) {
        val current = _state.value
        val position = current.pendingAnnotation ?: return
        val annotation = Annotation(
            id = ids.newId(),
            title = title.ifBlank { "Note" },
            body = body,
            position = position,
            normal = current.pendingAnnotationNormal,
            createdAtMillis = System.currentTimeMillis(),
        )
        viewModelScope.launch {
            projects.upsertAnnotation(projectId, annotation)
            _state.value = _state.value.copy(
                pendingAnnotation = null,
                pendingAnnotationNormal = null,
            )
        }
    }

    fun cancelAnnotation() {
        _state.value = _state.value.copy(
            pendingAnnotation = null,
            pendingAnnotationNormal = null,
        )
    }

    fun deleteAnnotation(id: String) = viewModelScope.launch {
        projects.deleteAnnotation(id)
    }

    // ----------------------------------------------------------------- edits

    fun applyEdit(edit: ModelEdit) {
        val next = _state.value.edits + edit
        _state.value = _state.value.copy(edits = next)
        persistEdits(next)
    }

    fun undoEdit() {
        val next = _state.value.edits.dropLast(1)
        _state.value = _state.value.copy(edits = next)
        persistEdits(next)
    }

    /**
     * Rescale from a saved measurement.
     *
     * The common case for a scan without a depth sensor: the geometry is right
     * but the scale is a guess, and the fix is to measure something you know
     * the size of and tell the app what it really is.
     */
    fun rescaleFrom(measurementId: String, actualMetres: Double) {
        val measured = _state.value.measurements.firstOrNull { it.id == measurementId }
        val metres = when (measured) {
            is Measurement.Distance -> measured.metres
            is Measurement.Height -> measured.metres
            else -> null
        } ?: return
        if (metres <= MIN_RESCALE_METRES || actualMetres <= MIN_RESCALE_METRES) return
        applyEdit(ModelEdit.Rescale(measuredMetres = metres, actualMetres = actualMetres))
    }

    private fun persistEdits(edits: List<ModelEdit>) = viewModelScope.launch {
        val project = _state.value.project ?: return@launch
        projects.upsert(project.copy(edits = edits))
    }

    private companion object {
        /**
         * How far the brightness slider goes either way.
         *
         * Two stops: enough to rescue a scan taken in a dim room, not enough
         * to turn a good one into something that no longer matches reality.
         */
        const val EXPOSURE_LIMIT_STOPS = 2f

        /**
         * Shortest length that can be rescaled from, in metres.
         *
         * A micron-long measurement is a mis-tap, and dividing by it would
         * scale the model to something no viewer could draw.
         */
        const val MIN_RESCALE_METRES = 1e-6
    }

    private fun labelFor(tool: MeasureTool, existing: List<Measurement>): String {
        val sameKind = existing.count { measurement ->
            when (tool) {
                MeasureTool.Distance -> measurement is Measurement.Distance
                MeasureTool.Height -> measurement is Measurement.Height
                MeasureTool.Area -> measurement is Measurement.Area
            }
        }
        return "${tool.label} ${sameKind + 1}"
    }
}
