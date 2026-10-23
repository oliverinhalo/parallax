package uk.co.jacoblevy.parallax

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.model.AppSettings
import uk.co.jacoblevy.parallax.core.model.ThemeMode
import javax.inject.Inject

/** What the activity needs before it can draw anything. */
sealed interface AppUiState {
    /**
     * Settings have not been read yet.
     *
     * The splash screen stays up for exactly this long. Drawing with default settings and then
     * switching to the user's choice would flash the wrong theme on every cold start.
     */
    data object Loading : AppUiState

    data class Ready(val settings: AppSettings, val onboardingComplete: Boolean) : AppUiState
}

@HiltViewModel
class AppViewModel @Inject constructor(
    settingsRepository: SettingsRepository,
) : ViewModel() {

    val state: StateFlow<AppUiState> = settingsRepository.settings
        .map<AppSettings, AppUiState> { settings ->
            AppUiState.Ready(
                settings = settings,
                onboardingComplete = settings.onboardingComplete,
            )
        }
        .stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(SUBSCRIPTION_TIMEOUT_MILLIS),
            initialValue = AppUiState.Loading,
        )

    private companion object {
        /**
         * Keep the upstream alive briefly after the last collector leaves, so a configuration
         * change does not re-read DataStore and drop back to [AppUiState.Loading] -- which would
         * show the splash again on every rotation.
         */
        const val SUBSCRIPTION_TIMEOUT_MILLIS = 5_000L
    }
}

/** Whether to draw the dark scheme, given the setting and the system. */
fun AppSettings.useDarkTheme(systemInDarkTheme: Boolean): Boolean = when (theme) {
    ThemeMode.Dark -> true
    ThemeMode.Light -> false
    ThemeMode.System -> systemInDarkTheme
}
