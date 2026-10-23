package uk.co.jacoblevy.parallax.feature.onboarding

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilityProvider
import javax.inject.Inject

/** One screen of the introduction. */
enum class OnboardingPage {
    /** What the app does, and the one thing that distinguishes it: nothing leaves the device. */
    Welcome,

    /** The four capture modes, so the first scan is not a guess. */
    Modes,

    /** How to move the phone. This is the single biggest determinant of scan quality. */
    Technique,

    /** The camera permission, asked with its reason visible rather than cold. */
    Permission,
}

data class OnboardingUiState(
    val page: OnboardingPage = OnboardingPage.Welcome,
    val capabilities: DeviceCapabilities = DeviceCapabilities(),
    val cameraGranted: Boolean = false,
    val permissionPermanentlyDenied: Boolean = false,
    val finished: Boolean = false,
) {
    val pageIndex: Int get() = page.ordinal
    val pageCount: Int get() = OnboardingPage.entries.size
    val isLastPage: Boolean get() = page == OnboardingPage.entries.last()

    /** The camera is the one hard requirement; everything else degrades. */
    val canFinish: Boolean get() = cameraGranted
}

sealed interface OnboardingEvent {
    data object Next : OnboardingEvent
    data object Back : OnboardingEvent
    data object Skip : OnboardingEvent
    data class PermissionResult(val granted: Boolean, val canAskAgain: Boolean) : OnboardingEvent
    data object Finish : OnboardingEvent
}

@HiltViewModel
class OnboardingViewModel @Inject constructor(
    private val settings: SettingsRepository,
    private val capabilities: DeviceCapabilityProvider,
) : ViewModel() {

    private val local = MutableStateFlow(OnboardingUiState())
    val state: StateFlow<OnboardingUiState> = local.asStateFlow()

    init {
        viewModelScope.launch {
            local.update { it.copy(capabilities = capabilities.resolve()) }
        }
    }

    fun onEvent(event: OnboardingEvent) {
        when (event) {
            OnboardingEvent.Next -> local.update { current ->
                val next = OnboardingPage.entries.getOrNull(current.pageIndex + 1)
                current.copy(page = next ?: current.page)
            }
            OnboardingEvent.Back -> local.update { current ->
                val previous = OnboardingPage.entries.getOrNull(current.pageIndex - 1)
                current.copy(page = previous ?: current.page)
            }
            // Skip jumps to the permission page rather than out of onboarding: the camera
            // permission is not optional, so skipping past it would land the user on a capture
            // screen that cannot start.
            OnboardingEvent.Skip -> local.update { it.copy(page = OnboardingPage.Permission) }
            is OnboardingEvent.PermissionResult -> local.update {
                it.copy(
                    cameraGranted = event.granted,
                    permissionPermanentlyDenied = !event.granted && !event.canAskAgain,
                )
            }
            OnboardingEvent.Finish -> {
                viewModelScope.launch {
                    settings.setOnboardingComplete(true)
                    local.update { it.copy(finished = true) }
                }
            }
        }
    }
}
