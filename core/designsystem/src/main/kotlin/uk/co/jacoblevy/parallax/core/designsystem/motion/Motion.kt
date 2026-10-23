package uk.co.jacoblevy.parallax.core.designsystem.motion

import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.Easing
import androidx.compose.animation.core.FiniteAnimationSpec
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize

/**
 * Motion tokens.
 *
 * Transitions between screens are spring-based rather than duration-based. A spring carries the
 * velocity of the gesture that started it, so a flick and a slow drag feel different in the way
 * the user already expects; a fixed-duration tween feels identical regardless of input and is
 * what makes an interface feel like it is playing animations at you.
 *
 * Durations are reserved for things with no gesture behind them: a progress bar advancing, a
 * prompt fading in.
 */
object Motion {

    object Duration {
        const val INSTANT = 90
        const val QUICK = 150
        const val STANDARD = 250
        const val EMPHASISED = 400
        const val SLOW = 650
    }

    object Easings {
        /** Material 3's emphasised decelerate: fast out, settles gently. */
        val emphasised: Easing = CubicBezierEasing(0.05f, 0.7f, 0.1f, 1.0f)
        val standard: Easing = CubicBezierEasing(0.2f, 0.0f, 0.0f, 1.0f)
        val accelerate: Easing = CubicBezierEasing(0.3f, 0.0f, 0.8f, 0.15f)
        /** For values that should never overshoot, such as a progress bar. */
        val linearish: Easing = CubicBezierEasing(0.4f, 0.0f, 0.6f, 1.0f)
    }

    /** Spring for a surface entering or leaving: no visible bounce, quick settle. */
    fun <T> enterSpring(): FiniteAnimationSpec<T> = spring(
        dampingRatio = 0.86f,
        stiffness = 420f,
    )

    /** Spring with a little life, for an element the user directly manipulated. */
    fun <T> responsiveSpring(): FiniteAnimationSpec<T> = spring(
        dampingRatio = 0.72f,
        stiffness = 700f,
    )

    /** Shared-element transitions from a library thumbnail into the viewer. */
    fun sharedBoundsSpec(): FiniteAnimationSpec<androidx.compose.ui.geometry.Rect> = spring(
        dampingRatio = 0.88f,
        stiffness = 380f,
    )

    fun sizeSpring(): FiniteAnimationSpec<IntSize> =
        spring(dampingRatio = 0.88f, stiffness = 380f, visibilityThreshold = IntSize(1, 1))

    fun offsetSpring(): FiniteAnimationSpec<IntOffset> =
        spring(dampingRatio = 0.88f, stiffness = 380f, visibilityThreshold = IntOffset(1, 1))

    /** Fade for text and icons, where a spring would look wobbly. */
    fun <T> fade(durationMillis: Int = Duration.STANDARD): FiniteAnimationSpec<T> =
        tween(durationMillis = durationMillis, easing = Easings.standard)

    /** Progress animation: monotonic, no overshoot, so the bar never appears to go backwards. */
    fun <T> progress(): FiniteAnimationSpec<T> =
        tween(durationMillis = Duration.EMPHASISED, easing = Easings.linearish)

    /** The slow, continuous drift of the processing point-cloud visual. */
    const val PROCESSING_ORBIT_MILLIS = 14_000

    /** Guidance prompts hold for at least this long so they can be read. */
    const val PROMPT_MIN_VISIBLE_MILLIS = 1_400L

    /** Spring used by the pinch/pan/orbit inertia in the viewer. */
    val VIEWER_INERTIA_DAMPING = 0.92f
    const val VIEWER_FLING_STIFFNESS = Spring.StiffnessLow
}
