package uk.co.jacoblevy.parallax.feature.capture

import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.toParallaxError
import uk.co.jacoblevy.parallax.core.data.repository.IdGenerator
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.CaptureStats
import uk.co.jacoblevy.parallax.core.model.CoverageMap
import uk.co.jacoblevy.parallax.core.model.DetectedPlane
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilityProvider
import uk.co.jacoblevy.parallax.core.model.GuidancePrompt
import uk.co.jacoblevy.parallax.core.model.HapticCue
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.feature.capture.logic.CaptureBudget
import uk.co.jacoblevy.parallax.feature.capture.session.ArSessionFailure
import uk.co.jacoblevy.parallax.feature.capture.session.CaptureEngine
import uk.co.jacoblevy.parallax.feature.capture.session.CaptureTick
import uk.co.jacoblevy.parallax.feature.capture.session.DeviceConditionMonitor
import uk.co.jacoblevy.parallax.feature.capture.session.FrameRecorder
import uk.co.jacoblevy.parallax.feature.capture.session.ImportProgress
import uk.co.jacoblevy.parallax.feature.capture.session.MediaImporter
import javax.inject.Inject

/** Where the capture screen is in its lifecycle. */
enum class CapturePhase {
    /** Choosing a mode, before anything is recorded. */
    Ready,

    /** Recording. */
    Capturing,

    /** Paused, with the frames so far kept. */
    Paused,

    /** Finished, with the manifest written; the screen is leaving. */
    Finished,
}

data class CaptureUiState(
    val phase: CapturePhase = CapturePhase.Ready,
    val mode: CaptureMode = CaptureMode.Object,
    val availableModes: List<CaptureMode> = CaptureMode.entries,
    val quality: QualityPreset = QualityPreset.Balanced,
    val capabilities: DeviceCapabilities = DeviceCapabilities(),
    val stats: CaptureStats = CaptureStats(),
    val coverage: CoverageMap = CoverageMap(),
    val planes: List<DetectedPlane> = emptyList(),
    val prompt: GuidancePrompt? = null,
    val projectId: String? = null,
    val sessionFailure: ArSessionFailure? = null,
    /** What threw, for the failures whose cause is not in their name. */
    val sessionFailureDetail: String? = null,
    val error: ParallaxError? = null,
    val hapticsEnabled: Boolean = true,
    val showAdvancedStats: Boolean = false,
    val importProgress: ImportProgress? = null,

    /** Set once the capture is complete and the project is ready to process. */
    val finishedProjectId: String? = null,
    val estimatedProcessingMillis: Long = 0,
    val estimatedProcessingBytes: Long = 0,
) {
    val isRecording: Boolean get() = phase == CapturePhase.Capturing

    /** Enough frames and coverage that finishing will produce something. */
    val canFinish: Boolean
        get() = stats.keyframeCount >= CaptureBudget.MIN_USEFUL_FRAMES

    /** The mode needs AR, and AR is not available. */
    val blockedByArCore: Boolean
        get() = mode.requiresArCore && !capabilities.arCoreAvailability.isUsable
}

sealed interface CaptureEvent {
    data class ModeSelected(val mode: CaptureMode) : CaptureEvent
    data class QualitySelected(val preset: QualityPreset) : CaptureEvent
    data object Start : CaptureEvent
    data object Pause : CaptureEvent
    data object Resume : CaptureEvent
    data object Finish : CaptureEvent
    data object Discard : CaptureEvent

    /** The user tapped the viewfinder to say what they are scanning. */
    data class SubjectTapped(val x: Float, val y: Float, val z: Float) : CaptureEvent

    /** Photos or videos picked from the gallery, for Import mode. */
    data class ImportMedia(val uris: List<Uri>) : CaptureEvent

    data object DismissError : CaptureEvent
}

/**
 * Drives a capture session.
 *
 * The frame loop itself is not here. ARCore's frames have to be consumed on the GL thread that
 * owns the session's texture, so the renderer calls [onFrameProcessed] with the result of each
 * tick and this class owns everything else: which mode, which project, the device conditions,
 * the haptics, and writing the manifest at the end.
 */
@HiltViewModel
class CaptureViewModel @Inject constructor(
    private val projects: ProjectRepository,
    private val storage: ProjectStorage,
    private val settings: SettingsRepository,
    private val capabilities: DeviceCapabilityProvider,
    private val conditions: DeviceConditionMonitor,
    private val ids: IdGenerator,
    private val importer: MediaImporter,
    private val haptics: HapticPlayer,
) : ViewModel() {

    private val local = MutableStateFlow(CaptureUiState())
    val state: StateFlow<CaptureUiState> = local.asStateFlow()

    /** The engine, created when a session starts and cleared when it ends. */
    @Volatile
    var engine: CaptureEngine? = null
        private set

    init {
        viewModelScope.launch {
            val resolved = capabilities.resolve()
            val stored = settings.current()
            local.update { current ->
                val modes = resolved.availableCaptureModes
                current.copy(
                    capabilities = resolved,
                    availableModes = modes,
                    // The stored default is honoured only if this device can actually do it.
                    mode = stored.defaultCaptureMode.takeIf { it in modes } ?: modes.first(),
                    quality = minOf(stored.defaultQuality, resolved.maxQualityPreset),
                    hapticsEnabled = stored.haptics,
                    showAdvancedStats = stored.showAdvancedStats,
                )
            }
        }

        conditions.conditions()
            .onEach { current ->
                engine?.updateConditions(current)
                local.update { state ->
                    state.copy(
                        stats = state.stats.copy(
                            thermalStatus = current.thermalStatus,
                            batteryPercent = current.batteryPercent,
                            storageAvailableBytes = current.storageAvailableBytes,
                        ),
                    )
                }
            }
            .launchIn(viewModelScope)
    }

    fun onEvent(event: CaptureEvent) {
        when (event) {
            is CaptureEvent.ModeSelected -> if (local.value.phase == CapturePhase.Ready) {
                local.update {
                    it.copy(mode = event.mode, sessionFailure = null, sessionFailureDetail = null)
                }
            }
            is CaptureEvent.QualitySelected -> local.update { it.copy(quality = event.preset) }
            CaptureEvent.Start -> startSession()
            CaptureEvent.Pause -> {
                local.update { it.copy(phase = CapturePhase.Paused) }
                haptics.play(HapticCue.Hint, local.value.hapticsEnabled)
            }
            CaptureEvent.Resume -> local.update { it.copy(phase = CapturePhase.Capturing) }
            CaptureEvent.Finish -> finishSession()
            CaptureEvent.Discard -> discardSession()
            is CaptureEvent.SubjectTapped -> {
                engine?.setSubjectCentre(event.x, event.y, event.z)
                haptics.play(HapticCue.Hint, local.value.hapticsEnabled)
            }
            is CaptureEvent.ImportMedia -> importMedia(event.uris)
            CaptureEvent.DismissError -> local.update {
                it.copy(error = null, sessionFailure = null, sessionFailureDetail = null)
            }
        }
    }

    /** Called by the renderer after each processed frame. Must be cheap. */
    fun onFrameProcessed(tick: CaptureTick) {
        local.update { current ->
            current.copy(
                stats = tick.stats.copy(
                    // The conditions come from the monitor, which is authoritative and updates
                    // on its own schedule; the engine's copy can be a frame behind.
                    thermalStatus = current.stats.thermalStatus,
                    batteryPercent = current.stats.batteryPercent,
                    storageAvailableBytes = current.stats.storageAvailableBytes,
                ),
                coverage = tick.coverage,
                prompt = tick.prompt,
                planes = tick.planes,
            )
        }
        if (tick.promptChanged) {
            val cue = tick.prompt?.haptic ?: HapticCue.None
            haptics.play(cue, local.value.hapticsEnabled)
        }
        if (tick.recorded != null) {
            engine?.updatePlanes(tick.planes)
        }
    }

    fun onSessionFailure(failure: ArSessionFailure, detail: String? = null) {
        local.update {
            it.copy(
                sessionFailure = failure,
                sessionFailureDetail = detail,
                phase = CapturePhase.Ready,
            )
        }
    }

    /** The screen is going away; release everything that holds the camera. */
    fun onStopped() {
        if (local.value.phase == CapturePhase.Capturing) {
            local.update { it.copy(phase = CapturePhase.Paused) }
        }
    }

    private fun startSession() {
        val current = local.value
        if (current.blockedByArCore) return
        viewModelScope.launch {
            runCatching {
                val projectId = current.projectId ?: ids.newId()
                if (current.projectId == null) {
                    projects.create(
                        id = projectId,
                        name = defaultName(current.mode),
                        mode = current.mode,
                        quality = current.quality,
                    )
                }
                val created = CaptureEngine(
                    mode = current.mode,
                    budget = CaptureBudget(current.capabilities, current.mode, current.quality),
                    recorder = FrameRecorder(storage, projectId),
                )
                created.updateConditions(conditions.current())
                created.start(System.currentTimeMillis())
                engine = created
                local.update {
                    it.copy(
                        projectId = projectId,
                        phase = CapturePhase.Capturing,
                        sessionFailure = null,
                        error = null,
                    )
                }
                haptics.play(HapticCue.Shutter, current.hapticsEnabled)
            }.onFailure { failure ->
                local.update { it.copy(error = failure.toParallaxError()) }
            }
        }
    }

    private fun finishSession() {
        val current = local.value
        val running = engine
        val projectId = current.projectId
        if (running == null || projectId == null) return

        local.update { it.copy(phase = CapturePhase.Finished) }
        viewModelScope.launch {
            runCatching {
                val summary = running.summarise(System.currentTimeMillis())
                projects.finishCapture(
                    id = projectId,
                    summary = summary,
                    frames = running.frames,
                )
                engine = null
                local.update {
                    it.copy(
                        finishedProjectId = projectId,
                        estimatedProcessingMillis =
                        it.quality.estimatedMillis(summary.keyframeCount),
                        estimatedProcessingBytes = it.quality.estimatedBytes(
                            keyframes = summary.keyframeCount,
                            sceneExtentMetres = summary.sceneExtentMetres,
                        ),
                    )
                }
                haptics.play(HapticCue.Success, current.hapticsEnabled)
            }.onFailure { failure ->
                // The frames are still on disk, so the capture is recoverable: the project
                // stays in its capturing state and the library offers to finish it.
                local.update {
                    it.copy(phase = CapturePhase.Paused, error = failure.toParallaxError())
                }
            }
        }
    }

    /**
     * Build a capture from gallery media.
     *
     * Runs to completion rather than streaming into the live loop: there are no poses to track
     * and no guidance to give, so the whole job is read, score, de-duplicate, write -- and the
     * user wants a progress bar, not a viewfinder.
     */
    private fun importMedia(uris: List<Uri>) {
        if (uris.isEmpty()) return
        val current = local.value
        viewModelScope.launch {
            local.update { it.copy(phase = CapturePhase.Capturing, error = null) }
            runCatching {
                val projectId = current.projectId ?: ids.newId()
                if (current.projectId == null) {
                    projects.create(
                        id = projectId,
                        name = defaultName(CaptureMode.Import),
                        mode = CaptureMode.Import,
                        quality = current.quality,
                    )
                }
                local.update { it.copy(projectId = projectId) }

                val result = importer.import(
                    projectId = projectId,
                    uris = uris,
                    preset = current.quality,
                ) { progress ->
                    local.update { it.copy(importProgress = progress) }
                }
                projects.finishCapture(projectId, result.summary, result.frames)
                local.update {
                    it.copy(
                        phase = CapturePhase.Finished,
                        importProgress = null,
                        finishedProjectId = projectId,
                        estimatedProcessingMillis =
                        it.quality.estimatedMillis(result.summary.keyframeCount),
                        estimatedProcessingBytes = it.quality.estimatedBytes(
                            keyframes = result.summary.keyframeCount,
                            sceneExtentMetres = result.summary.sceneExtentMetres,
                        ),
                    )
                }
                haptics.play(HapticCue.Success, current.hapticsEnabled)
            }.onFailure { failure ->
                // The partially written frames are deleted: an import that failed because the
                // set was too small must not leave a project that cannot be processed.
                local.value.projectId?.let { id -> runCatching { projects.delete(id) } }
                local.update {
                    it.copy(
                        phase = CapturePhase.Ready,
                        projectId = null,
                        importProgress = null,
                        error = failure.toParallaxError(),
                    )
                }
            }
        }
    }

    private fun discardSession() {
        val projectId = local.value.projectId
        engine?.discard()
        engine = null
        viewModelScope.launch {
            runCatching { if (projectId != null) projects.delete(projectId) }
            local.value = local.value.copy(
                phase = CapturePhase.Ready,
                projectId = null,
                stats = CaptureStats(),
                coverage = CoverageMap(),
                prompt = null,
            )
        }
    }

    private fun defaultName(mode: CaptureMode): String = when (mode) {
        CaptureMode.Object -> "Object scan"
        CaptureMode.Room -> "Room scan"
        CaptureMode.Area -> "Area scan"
        CaptureMode.Import -> "Imported scan"
    }

    override fun onCleared() {
        // A capture abandoned by the process going away leaves its frames on disk and its
        // project in the capturing state, which is what lets the library offer to resume it.
        engine = null
    }
}
