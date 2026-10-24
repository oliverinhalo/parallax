package uk.co.jacoblevy.parallax.core.designsystem

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onRoot
import com.github.takahirom.roborazzi.captureRoboImage
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.designsystem.component.BadgeTone
import uk.co.jacoblevy.parallax.core.designsystem.component.CompactStat
import uk.co.jacoblevy.parallax.core.designsystem.component.EmptyState
import uk.co.jacoblevy.parallax.core.designsystem.component.ErrorState
import uk.co.jacoblevy.parallax.core.designsystem.component.GlassPanel
import uk.co.jacoblevy.parallax.core.designsystem.component.LabelledProgress
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxBadge
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxCard
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxChip
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxDivider
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxOutlinedButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxProgressBar
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxSection
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxTextButton
import uk.co.jacoblevy.parallax.core.designsystem.component.StatReadout
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing

/**
 * The design system, drawn.
 *
 * Every component in one gallery rather than one test each: the thing worth
 * catching is a change to a shared token -- a radius, a spacing step, an accent
 * -- and a single image shows all of it at once. One test per component would
 * make the same change produce twenty failures and no clearer a picture.
 *
 * Recorded with `./gradlew recordRoborazziDebug` and verified in CI with
 * `verifyRoborazziDebug`.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
/**
 * Pinned to API 35.
 *
 * Not compileSdk: Compose's test rule reflects on `InputManager.getInstance()`,
 * which newer Android images no longer have, so a screenshot test on the
 * newest image fails before it draws anything. 35 is the newest where Compose
 * and Robolectric agree. The device qualifiers are a mid-size phone in
 * portrait, which is what this app is used on.
 */
@Config(sdk = [35], qualifiers = "w411dp-h891dp-xxhdpi")
class DesignSystemScreenshotTest {

    @get:Rule
    val compose = createComposeRule()

    @Test
    fun gallery() = capture("design-system-dark") { Gallery() }

    /**
     * The same gallery at the largest font scale the platform offers.
     *
     * This is where a fixed-height button or a single-line label with no
     * ellipsis shows up, and it is the accessibility regression that is
     * easiest to introduce and hardest to notice.
     */
    @Test
    fun galleryAtLargestFontScale() =
        capture("design-system-large-text", fontScale = 2.0f) { Gallery() }

    @Test
    fun states() = capture("design-system-states") {
        Column(
            Modifier.fillMaxWidth().padding(Spacing.lg),
            verticalArrangement = Arrangement.spacedBy(Spacing.xl),
        ) {
            EmptyState(
                title = "No scans yet",
                message = "Everything you capture stays on this device.",
                icon = ParallaxIcons.Scan,
                actionLabel = "Start scanning",
                onAction = {},
            )
            ParallaxDivider()
            // A real error type rather than loose strings: ErrorState's whole
            // point is that the recovery action comes from the error.
            ErrorState(
                error = ParallaxError.InsufficientStorage(
                    requiredBytes = 2L * 1024 * 1024 * 1024,
                    availableBytes = 400L * 1024 * 1024,
                ),
                icon = ParallaxIcons.Warning,
            )
        }
    }

    private fun capture(
        name: String,
        fontScale: Float = 1.0f,
        content: @Composable () -> Unit,
    ) {
        compose.setContent {
            androidx.compose.runtime.CompositionLocalProvider(
                androidx.compose.ui.platform.LocalDensity provides
                    androidx.compose.ui.unit.Density(
                        density = androidx.compose.ui.platform.LocalDensity.current.density,
                        fontScale = fontScale,
                    ),
            ) {
                ParallaxTheme(darkTheme = true, dynamicColor = false) { content() }
            }
        }
        compose.onRoot().captureRoboImage("src/test/screenshots/$name.png")
    }

    @Composable
    private fun Gallery() {
        Column(
            Modifier.fillMaxWidth().padding(Spacing.lg),
            verticalArrangement = Arrangement.spacedBy(Spacing.lg),
        ) {
            ParallaxSection(title = "Buttons") {
                Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                    ParallaxButton(text = "Start scanning", onClick = {})
                    ParallaxOutlinedButton(text = "Import photos", onClick = {})
                    ParallaxTextButton(text = "Not now", onClick = {})
                    ParallaxButton(text = "Unavailable", onClick = {}, enabled = false)
                }
            }

            ParallaxSection(title = "Chips and badges") {
                Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
                        verticalArrangement = Arrangement.spacedBy(Spacing.sm),
                    ) {
                        ParallaxChip(text = "Shaded", selected = true, onClick = {})
                        ParallaxChip(text = "Points", selected = false, onClick = {})
                        ParallaxChip(text = "Splat", selected = false, onClick = {}, enabled = false)
                    }
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
                        verticalArrangement = Arrangement.spacedBy(Spacing.sm),
                    ) {
                        for (tone in BadgeTone.entries) {
                            ParallaxBadge(text = tone.name, tone = tone)
                        }
                    }
                }
            }

            ParallaxSection(title = "Progress") {
                Column(verticalArrangement = Arrangement.spacedBy(Spacing.md)) {
                    ParallaxProgressBar(progress = 0.42f, modifier = Modifier.fillMaxWidth())
                    ParallaxProgressBar(
                        progress = 0f,
                        indeterminate = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    LabelledProgress(
                        label = "Fusing surfaces",
                        detail = "4 of 11 · about 6 min left",
                        progress = 0.36f,
                    )
                }
            }

            ParallaxSection(title = "Readouts") {
                Column(verticalArrangement = Arrangement.spacedBy(Spacing.md)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(Spacing.lg)) {
                        StatReadout(label = "Triangles", value = "284,112")
                        StatReadout(label = "Floor area", value = "18.4 m²")
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(Spacing.lg)) {
                        CompactStat(label = "Frames", value = "162")
                        CompactStat(label = "Size", value = "248 MB")
                    }
                }
            }

            ParallaxSection(title = "Surfaces") {
                Column(verticalArrangement = Arrangement.spacedBy(Spacing.md)) {
                    ParallaxCard(onClick = {}) {
                        Text("A tappable card", Modifier.padding(Spacing.md))
                    }
                    GlassPanel(Modifier.fillMaxWidth()) {
                        Text("Glass, for overlays on the camera")
                    }
                }
            }
        }
    }
}
