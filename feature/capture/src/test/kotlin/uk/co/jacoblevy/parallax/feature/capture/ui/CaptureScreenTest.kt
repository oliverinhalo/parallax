package uk.co.jacoblevy.parallax.feature.capture.ui

import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.junit4.createComposeRule
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.GraphicsMode
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.model.ArCoreAvailability
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.feature.capture.CaptureUiState
import uk.co.jacoblevy.parallax.feature.capture.session.ArSessionFailure

/**
 * The capture screen composes.
 *
 * It is the one screen with a live camera in it, so it was also the one screen nothing ever
 * built: the viewfinder is hardware and the rest went untested with it. Composition is not the
 * camera, and this does not pretend otherwise — but a screen that throws while being built
 * never reaches the camera at all, and that is invisible to a ViewModel test.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class CaptureScreenTest {

    @get:Rule
    val compose = createComposeRule()

    private fun composeWith(initial: CaptureUiState): MutableState<CaptureUiState> {
        val state = mutableStateOf(initial)
        compose.setContent {
            ParallaxTheme {
                CaptureScreen(
                    state = state.value,
                    onEvent = {},
                    onCancelled = {},
                    engineProvider = { null },
                    onTick = {},
                    onSessionFailure = {},
                )
            }
        }
        compose.waitForIdle()
        return state
    }

    @Test
    fun `the screen builds in the state it is first opened in`() {
        composeWith(CaptureUiState())
    }

    @Test
    fun `the screen builds for every capture mode`() {
        // One `setContent` per rule, so the mode is driven through state the way the app does
        // it -- which also exercises the recomposition, not only the first build.
        val state = composeWith(CaptureUiState())

        CaptureMode.entries.forEach { mode ->
            state.value = CaptureUiState(mode = mode)
            compose.waitForIdle()
        }
    }

    @Test
    fun `the screen builds when AR is unavailable and when a session has failed`() {
        val state = composeWith(CaptureUiState())

        state.value = CaptureUiState(
            mode = CaptureMode.Room,
            capabilities = DeviceCapabilities(arCoreAvailability = ArCoreAvailability.UnsupportedDevice),
        )
        compose.waitForIdle()

        state.value = CaptureUiState(sessionFailure = ArSessionFailure.CameraUnavailable)
        compose.waitForIdle()
    }
}
