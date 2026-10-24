package uk.co.jacoblevy.parallax.core.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.LengthUnit
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ThemeMode
import java.io.File

@RunWith(RobolectricTestRunner::class)
class SettingsRepositoryTest {

    private lateinit var store: DataStore<Preferences>
    private lateinit var repository: SettingsRepository

    @Before
    fun setUp() {
        val dir = File(
            ApplicationProvider.getApplicationContext<android.content.Context>().filesDir,
            "settings-test",
        ).apply { mkdirs() }
        File(dir, "test.preferences_pb").delete()
        store = PreferenceDataStoreFactory.create { File(dir, "test.preferences_pb") }
        repository = SettingsRepository(store)
    }

    @Test
    fun `defaults are dark, metric, balanced, and brand-coloured`() = runTest {
        val settings = repository.current()
        assertThat(settings.theme).isEqualTo(ThemeMode.Dark)
        assertThat(settings.units).isEqualTo(LengthUnit.Metres)
        assertThat(settings.defaultQuality).isEqualTo(QualityPreset.Balanced)
        // Material You is opt-in: the brand palette is the app's identity.
        assertThat(settings.useDynamicColor).isFalse()
        assertThat(settings.haptics).isTrue()
        assertThat(settings.onboardingComplete).isFalse()
    }

    @Test
    fun `each setting persists independently`() = runTest {
        repository.setTheme(ThemeMode.System)
        repository.setUnits(LengthUnit.Feet)
        repository.setDefaultCaptureMode(CaptureMode.Room)
        repository.setKeepRawFrames(true)
        repository.setStorageBudgetBytes(5_000_000_000)

        val settings = repository.settings.first()
        assertThat(settings.theme).isEqualTo(ThemeMode.System)
        assertThat(settings.units).isEqualTo(LengthUnit.Feet)
        assertThat(settings.imperial).isTrue()
        assertThat(settings.defaultCaptureMode).isEqualTo(CaptureMode.Room)
        assertThat(settings.keepRawFramesAfterProcessing).isTrue()
        assertThat(settings.storageBudgetBytes).isEqualTo(5_000_000_000)
        // Untouched settings keep their defaults rather than being overwritten by a blob write.
        assertThat(settings.defaultQuality).isEqualTo(QualityPreset.Balanced)
    }

    @Test
    fun `a negative storage budget is clamped rather than stored`() = runTest {
        repository.setStorageBudgetBytes(-1)
        assertThat(repository.current().storageBudgetBytes).isEqualTo(0)
    }

    @Test
    fun `an unrecognised enum name falls back to the default`() = runTest {
        repository.setTheme(ThemeMode.Light)
        store.updateData { prefs ->
            prefs.toMutablePreferences().apply {
                this[androidx.datastore.preferences.core.stringPreferencesKey("theme")] = "Sepia"
            }
        }
        assertThat(repository.current().theme).isEqualTo(ThemeMode.Dark)
    }

    @Test
    fun `resetting keeps the onboarding flags`() = runTest {
        repository.setOnboardingComplete(true)
        repository.setTutorialComplete(true)
        repository.setUnits(LengthUnit.Inches)

        repository.resetToDefaults()

        val settings = repository.current()
        assertThat(settings.units).isEqualTo(LengthUnit.Metres)
        // Showing the tour again to someone who has finished it would read as a bug.
        assertThat(settings.onboardingComplete).isTrue()
        assertThat(settings.tutorialComplete).isTrue()
    }
}
