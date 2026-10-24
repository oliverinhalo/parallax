package uk.co.jacoblevy.parallax.feature.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.toParallaxError
import uk.co.jacoblevy.parallax.core.common.util.formatBytes
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.AppSettings
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilityProvider
import uk.co.jacoblevy.parallax.core.model.LengthUnit
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.StorageBreakdown
import uk.co.jacoblevy.parallax.core.model.ThemeMode
import uk.co.jacoblevy.parallax.engine.nativebridge.ParallaxEngine
import javax.inject.Inject

data class SettingsUiState(
    val settings: AppSettings = AppSettings(),
    val capabilities: DeviceCapabilities = DeviceCapabilities(),
    val storage: StorageBreakdown = StorageBreakdown(),
    val projectCount: Int = 0,
    /** Projects whose raw frames can still be discarded, with how much that would free. */
    val reclaimableProjects: Int = 0,
    val engineVersion: String = "",
    val engineUnavailableReason: String? = null,
    val busy: Boolean = false,
    val error: ParallaxError? = null,
    val notice: String? = null,
    val confirmingPurge: Boolean = false,
    val confirmingReset: Boolean = false,
)

/**
 * Everything the settings screen can ask for.
 *
 * The toggles and choices are grouped apart from the two destructive actions, so the ViewModel
 * dispatches a short `when` per group instead of one twenty-branch block. The concrete names are
 * unchanged.
 */
sealed interface SettingsEvent {

    /** A single stored preference. Each one writes exactly one DataStore key. */
    sealed interface Preference : SettingsEvent

    /** Actions that delete data or reset state, and are therefore confirmed first. */
    sealed interface Destructive : SettingsEvent

    /** Transient messages the screen has shown and is done with. */
    sealed interface Dismissing : SettingsEvent

    data class SetTheme(val mode: ThemeMode) : Preference
    data class SetDynamicColor(val enabled: Boolean) : Preference
    data class SetUnits(val unit: LengthUnit) : Preference
    data class SetDefaultQuality(val preset: QualityPreset) : Preference
    data class SetDefaultMode(val mode: CaptureMode) : Preference
    data class SetHaptics(val enabled: Boolean) : Preference
    data class SetVoicePrompts(val enabled: Boolean) : Preference
    data class SetKeepRawFrames(val enabled: Boolean) : Preference
    data class SetChargingOnly(val enabled: Boolean) : Preference
    data class SetBackgroundSplats(val enabled: Boolean) : Preference
    data class SetAdvancedStats(val enabled: Boolean) : Preference
    data class SetStorageBudget(val bytes: Long) : Preference

    data object RequestPurgeIntermediates : Destructive
    data object ConfirmPurgeIntermediates : Destructive
    data object CancelPurgeIntermediates : Destructive
    data object RequestReset : Destructive
    data object ConfirmReset : Destructive
    data object CancelReset : Destructive

    data object DismissError : Dismissing
    data object DismissNotice : Dismissing
}

/**
 * Settings and the storage dashboard.
 *
 * The storage figures are read from disk rather than from the database's `storage_bytes` column.
 * The column is what the library sorts on and is updated on write; this screen is where a user
 * goes when the numbers look wrong, so it must not be able to repeat the same stale answer.
 */
@HiltViewModel
class SettingsViewModel @Inject constructor(
    private val settings: SettingsRepository,
    private val projects: ProjectRepository,
    private val storage: ProjectStorage,
    private val capabilities: DeviceCapabilityProvider,
) : ViewModel() {

    private val local = MutableStateFlow(SettingsUiState())
    val state: StateFlow<SettingsUiState> = local.asStateFlow()

    init {
        combine(settings.settings, projects.observeAll()) { appSettings, allProjects ->
            appSettings to allProjects
        }
            .onEach { (appSettings, allProjects) ->
                local.update { previous ->
                    previous.copy(
                        settings = appSettings,
                        projectCount = allProjects.size,
                        reclaimableProjects = allProjects.count(::canReclaim),
                    )
                }
            }
            .launchIn(viewModelScope)

        viewModelScope.launch {
            local.update {
                it.copy(
                    capabilities = capabilities.resolve(),
                    engineVersion = ParallaxEngine.version(),
                    engineUnavailableReason = ParallaxEngine.unavailableReason,
                )
            }
            refreshStorage()
        }
    }

    fun onEvent(event: SettingsEvent) {
        when (event) {
            is SettingsEvent.Preference -> onPreference(event)
            is SettingsEvent.Destructive -> onDestructive(event)
            is SettingsEvent.Dismissing -> onDismissing(event)
        }
    }

    private fun onPreference(event: SettingsEvent.Preference) = edit {
        when (event) {
            is SettingsEvent.SetTheme -> settings.setTheme(event.mode)
            is SettingsEvent.SetDynamicColor -> settings.setDynamicColor(event.enabled)
            is SettingsEvent.SetUnits -> settings.setUnits(event.unit)
            is SettingsEvent.SetDefaultQuality -> settings.setDefaultQuality(event.preset)
            is SettingsEvent.SetDefaultMode -> settings.setDefaultCaptureMode(event.mode)
            is SettingsEvent.SetHaptics -> settings.setHaptics(event.enabled)
            is SettingsEvent.SetVoicePrompts -> settings.setVoicePrompts(event.enabled)
            is SettingsEvent.SetKeepRawFrames -> settings.setKeepRawFrames(event.enabled)
            is SettingsEvent.SetChargingOnly ->
                settings.setProcessOnlyWhileCharging(event.enabled)
            is SettingsEvent.SetBackgroundSplats ->
                settings.setTrainSplatsInBackground(event.enabled)
            is SettingsEvent.SetAdvancedStats -> settings.setShowAdvancedStats(event.enabled)
            is SettingsEvent.SetStorageBudget -> settings.setStorageBudgetBytes(event.bytes)
        }
    }

    private fun onDestructive(event: SettingsEvent.Destructive) {
        when (event) {
            SettingsEvent.RequestPurgeIntermediates ->
                local.update { it.copy(confirmingPurge = true) }
            SettingsEvent.CancelPurgeIntermediates ->
                local.update { it.copy(confirmingPurge = false) }
            SettingsEvent.ConfirmPurgeIntermediates -> purgeIntermediates()

            SettingsEvent.RequestReset -> local.update { it.copy(confirmingReset = true) }
            SettingsEvent.CancelReset -> local.update { it.copy(confirmingReset = false) }
            SettingsEvent.ConfirmReset -> {
                local.update { it.copy(confirmingReset = false) }
                edit {
                    settings.resetToDefaults()
                    local.update { it.copy(notice = "Settings restored to defaults") }
                }
            }
        }
    }

    private fun purgeIntermediates() {
        local.update { it.copy(confirmingPurge = false, busy = true) }
        edit {
            var freed = 0L
            for (project in projects.observeAll().first()) {
                if (canReclaim(project)) freed += projects.purgeIntermediates(project.id)
            }
            refreshStorage()
            local.update { it.copy(notice = "Freed ${formatBytes(freed)}", busy = false) }
        }
    }

    private fun onDismissing(event: SettingsEvent.Dismissing) {
        when (event) {
            SettingsEvent.DismissError -> local.update { it.copy(error = null) }
            SettingsEvent.DismissNotice -> local.update { it.copy(notice = null) }
        }
    }

    /** Recompute the storage breakdown from the filesystem. */
    fun refresh() {
        viewModelScope.launch { refreshStorage() }
    }

    private suspend fun refreshStorage() {
        runCatching { storage.breakdown() }
            .onSuccess { breakdown -> local.update { it.copy(storage = breakdown) } }
            .onFailure { failure -> local.update { it.copy(error = failure.toParallaxError()) } }
    }

    private fun edit(block: suspend () -> Unit) {
        viewModelScope.launch {
            runCatching { block() }.onFailure { failure ->
                local.update { it.copy(busy = false, error = failure.toParallaxError()) }
            }
        }
    }

    /**
     * Whether a project's raw frames are still on disk and no longer needed.
     *
     * Only finished scans qualify. Purging an unprocessed capture's frames would destroy the
     * only copy of the data the model has not been built from yet.
     */
    private fun canReclaim(project: Project): Boolean =
        project.status is ProjectStatus.Ready &&
            storage.framesDir(project.id).listFiles()?.isNotEmpty() == true
}
