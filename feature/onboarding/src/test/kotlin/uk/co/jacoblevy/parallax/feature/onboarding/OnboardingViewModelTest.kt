package uk.co.jacoblevy.parallax.feature.onboarding

import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.test.runTest
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.model.ArCoreAvailability
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilityProvider
import uk.co.jacoblevy.parallax.core.testing.MainDispatcherRule
import java.io.File

@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
class OnboardingViewModelTest {

    @get:Rule
    val mainDispatcher = MainDispatcherRule()

    private lateinit var settings: SettingsRepository
    private var capabilities = DeviceCapabilities(
        arCoreAvailability = ArCoreAvailability.Supported,
    )

    @Before
    fun setUp() {
        val context = ApplicationProvider.getApplicationContext<android.content.Context>()
        val dir = File(context.filesDir, "onboarding-test").apply { mkdirs() }
        File(dir, "settings.preferences_pb").delete()
        settings = SettingsRepository(
            PreferenceDataStoreFactory.create(
                scope = CoroutineScope(mainDispatcher.dispatcher + Job()),
            ) { File(dir, "settings.preferences_pb") },
        )
    }

    private fun viewModel() = OnboardingViewModel(
        settings = settings,
        capabilities = object : DeviceCapabilityProvider {
            override suspend fun resolve(): DeviceCapabilities = capabilities
        },
    )

    private fun settled(model: OnboardingViewModel): OnboardingUiState {
        mainDispatcher.scheduler.advanceUntilIdle()
        return model.state.value
    }

    @Test
    fun `it starts on the welcome page and walks forwards and back`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        assertThat(settled(model).page).isEqualTo(OnboardingPage.Welcome)

        model.onEvent(OnboardingEvent.Next)
        assertThat(settled(model).page).isEqualTo(OnboardingPage.Modes)

        model.onEvent(OnboardingEvent.Back)
        assertThat(settled(model).page).isEqualTo(OnboardingPage.Welcome)
    }

    @Test
    fun `it cannot be walked past either end`() = runTest(mainDispatcher.dispatcher) {
        val model = viewModel()
        model.onEvent(OnboardingEvent.Back)
        assertThat(settled(model).page).isEqualTo(OnboardingPage.Welcome)

        repeat(OnboardingPage.entries.size + 3) { model.onEvent(OnboardingEvent.Next) }
        assertThat(settled(model).page).isEqualTo(OnboardingPage.Permission)
        assertThat(settled(model).isLastPage).isTrue()
    }

    @Test
    fun `skip lands on the permission page rather than leaving onboarding`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        model.onEvent(OnboardingEvent.Skip)

        // Skipping past the camera permission would drop the user on a capture screen that
        // cannot start, which reads as a broken app rather than a missing permission.
        assertThat(settled(model).page).isEqualTo(OnboardingPage.Permission)
    }

    @Test
    fun `onboarding cannot be finished without the camera`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        assertThat(settled(model).canFinish).isFalse()

        model.onEvent(OnboardingEvent.PermissionResult(granted = true, canAskAgain = true))
        assertThat(settled(model).canFinish).isTrue()
    }

    @Test
    fun `a permanent denial is distinguished from a refusal`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()

        model.onEvent(OnboardingEvent.PermissionResult(granted = false, canAskAgain = true))
        assertThat(settled(model).permissionPermanentlyDenied).isFalse()

        // Only now should the UI stop offering a button that Android will ignore.
        model.onEvent(OnboardingEvent.PermissionResult(granted = false, canAskAgain = false))
        assertThat(settled(model).permissionPermanentlyDenied).isTrue()
    }

    @Test
    fun `granting after a permanent denial clears the dead end`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        model.onEvent(OnboardingEvent.PermissionResult(granted = false, canAskAgain = false))
        // As if they granted it in the system settings and came back.
        model.onEvent(OnboardingEvent.PermissionResult(granted = true, canAskAgain = true))

        val state = settled(model)
        assertThat(state.permissionPermanentlyDenied).isFalse()
        assertThat(state.canFinish).isTrue()
    }

    @Test
    fun `finishing records the flag so onboarding is never shown again`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        model.onEvent(OnboardingEvent.PermissionResult(granted = true, canAskAgain = true))
        model.onEvent(OnboardingEvent.Finish)

        assertThat(settled(model).finished).isTrue()
        assertThat(settings.current().onboardingComplete).isTrue()
    }

    @Test
    fun `a device without ARCore is told which modes it has rather than shown all four`() =
        runTest(mainDispatcher.dispatcher) {
            capabilities = DeviceCapabilities(
                arCoreAvailability = ArCoreAvailability.UnsupportedDevice,
            )

            val state = settled(viewModel())

            assertThat(state.capabilities.availableCaptureModes)
                .containsExactly(CaptureMode.Import)
        }
}
