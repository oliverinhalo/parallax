package uk.co.jacoblevy.parallax.core.data.settings

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import uk.co.jacoblevy.parallax.core.model.AppSettings
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.LengthUnit
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ThemeMode
import javax.inject.Inject
import javax.inject.Singleton

/**
 * App settings, on Preferences DataStore.
 *
 * Stored as individual keys rather than one serialised blob so that adding a setting in a later
 * version cannot invalidate the ones already saved, and so a corrupt value affects one setting
 * instead of all of them. Enums are stored by name and read back leniently: a name this build
 * does not know falls back to the default rather than throwing on every read.
 */
@Singleton
class SettingsRepository @Inject constructor(
    private val dataStore: DataStore<Preferences>,
) {
    val settings: Flow<AppSettings> = dataStore.data
        // A read failure here would otherwise propagate to every screen, since the theme is read
        // before anything is drawn. Defaults are a far better outcome than a blank app.
        .catch { emit(androidx.datastore.preferences.core.emptyPreferences()) }
        .map { prefs ->
            AppSettings(
                theme = prefs.enum(Keys.THEME, ThemeMode.Dark),
                useDynamicColor = prefs[Keys.DYNAMIC_COLOR] ?: false,
                units = prefs.enum(Keys.UNITS, LengthUnit.Metres),
                defaultQuality = prefs.enum(Keys.QUALITY, QualityPreset.Balanced),
                defaultCaptureMode = prefs.enum(Keys.CAPTURE_MODE, CaptureMode.Object),
                haptics = prefs[Keys.HAPTICS] ?: true,
                guidanceVoicePrompts = prefs[Keys.VOICE_PROMPTS] ?: false,
                keepRawFramesAfterProcessing = prefs[Keys.KEEP_RAW_FRAMES] ?: false,
                processOnlyWhileCharging = prefs[Keys.CHARGING_ONLY] ?: false,
                processOnlyOnUnmeteredNetwork = prefs[Keys.UNMETERED_ONLY] ?: false,
                trainSplatsInBackground = prefs[Keys.BACKGROUND_SPLATS] ?: true,
                showAdvancedStats = prefs[Keys.ADVANCED_STATS] ?: false,
                onboardingComplete = prefs[Keys.ONBOARDING_DONE] ?: false,
                tutorialComplete = prefs[Keys.TUTORIAL_DONE] ?: false,
                storageBudgetBytes = prefs[Keys.STORAGE_BUDGET] ?: 0L,
            )
        }

    suspend fun current(): AppSettings = settings.first()

    suspend fun setTheme(mode: ThemeMode) = put(Keys.THEME, mode.name)
    suspend fun setDynamicColor(enabled: Boolean) = put(Keys.DYNAMIC_COLOR, enabled)
    suspend fun setUnits(unit: LengthUnit) = put(Keys.UNITS, unit.name)
    suspend fun setDefaultQuality(preset: QualityPreset) = put(Keys.QUALITY, preset.name)
    suspend fun setDefaultCaptureMode(mode: CaptureMode) = put(Keys.CAPTURE_MODE, mode.name)
    suspend fun setHaptics(enabled: Boolean) = put(Keys.HAPTICS, enabled)
    suspend fun setVoicePrompts(enabled: Boolean) = put(Keys.VOICE_PROMPTS, enabled)
    suspend fun setKeepRawFrames(enabled: Boolean) = put(Keys.KEEP_RAW_FRAMES, enabled)
    suspend fun setProcessOnlyWhileCharging(enabled: Boolean) = put(Keys.CHARGING_ONLY, enabled)
    suspend fun setProcessOnlyOnUnmetered(enabled: Boolean) = put(Keys.UNMETERED_ONLY, enabled)
    suspend fun setTrainSplatsInBackground(enabled: Boolean) = put(Keys.BACKGROUND_SPLATS, enabled)
    suspend fun setShowAdvancedStats(enabled: Boolean) = put(Keys.ADVANCED_STATS, enabled)
    suspend fun setOnboardingComplete(done: Boolean) = put(Keys.ONBOARDING_DONE, done)
    suspend fun setTutorialComplete(done: Boolean) = put(Keys.TUTORIAL_DONE, done)
    suspend fun setStorageBudgetBytes(bytes: Long) =
        put(Keys.STORAGE_BUDGET, bytes.coerceAtLeast(0))

    /** Restore defaults, keeping the onboarding flags so the user is not shown it again. */
    suspend fun resetToDefaults() {
        dataStore.edit { prefs ->
            val onboarding = prefs[Keys.ONBOARDING_DONE]
            val tutorial = prefs[Keys.TUTORIAL_DONE]
            prefs.clear()
            onboarding?.let { prefs[Keys.ONBOARDING_DONE] = it }
            tutorial?.let { prefs[Keys.TUTORIAL_DONE] = it }
        }
    }

    private suspend fun <T> put(key: Preferences.Key<T>, value: T) {
        dataStore.edit { it[key] = value }
    }

    private inline fun <reified T : Enum<T>> Preferences.enum(
        key: Preferences.Key<String>,
        fallback: T,
    ): T {
        val name = this[key] ?: return fallback
        return enumValues<T>().firstOrNull { it.name == name } ?: fallback
    }

    private object Keys {
        val THEME = stringPreferencesKey("theme")
        val DYNAMIC_COLOR = booleanPreferencesKey("dynamic_color")
        val UNITS = stringPreferencesKey("units")
        val QUALITY = stringPreferencesKey("default_quality")
        val CAPTURE_MODE = stringPreferencesKey("default_capture_mode")
        val HAPTICS = booleanPreferencesKey("haptics")
        val VOICE_PROMPTS = booleanPreferencesKey("voice_prompts")
        val KEEP_RAW_FRAMES = booleanPreferencesKey("keep_raw_frames")
        val CHARGING_ONLY = booleanPreferencesKey("charging_only")
        val UNMETERED_ONLY = booleanPreferencesKey("unmetered_only")
        val BACKGROUND_SPLATS = booleanPreferencesKey("background_splats")
        val ADVANCED_STATS = booleanPreferencesKey("advanced_stats")
        val ONBOARDING_DONE = booleanPreferencesKey("onboarding_complete")
        val TUTORIAL_DONE = booleanPreferencesKey("tutorial_complete")
        val STORAGE_BUDGET = longPreferencesKey("storage_budget_bytes")
    }

    companion object {
        const val STORE_NAME = "parallax_settings"
    }
}
