package uk.co.jacoblevy.parallax.feature.capture.logic

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.GuidancePrompt
import uk.co.jacoblevy.parallax.core.model.MotionBand
import uk.co.jacoblevy.parallax.core.model.TrackingState

/**
 * Telling the user which way they are getting it wrong.
 *
 * The screen could previously say "slow down" and nothing else, so standing still -- the
 * mistake that produces no scan at all -- looked exactly like doing it correctly.
 */
class MotionGuidanceTest {

    private fun inputs(
        speed: Float = 0.2f,
        recording: Boolean = true,
        millisSinceKeyframe: Long = 0,
        tracking: TrackingState = TrackingState.Tracking,
    ) = GuidanceInputs(
        mode = CaptureMode.Object,
        trackingState = tracking,
        quality = FrameQuality(acceptable = true),
        speedMetresPerSecond = speed,
        recording = recording,
        millisSinceKeyframe = millisSinceKeyframe,
        keyframeCount = 4,
    )

    @Test
    fun `standing still is its own band, not the absence of a warning`() {
        assertThat(MotionBand.of(0f)).isEqualTo(MotionBand.Still)
        assertThat(MotionBand.of(0.05f)).isEqualTo(MotionBand.Slow)
        assertThat(MotionBand.of(0.25f)).isEqualTo(MotionBand.Ideal)
        assertThat(MotionBand.of(0.5f)).isEqualTo(MotionBand.Fast)
        assertThat(MotionBand.of(1.2f)).isEqualTo(MotionBand.TooFast)
    }

    @Test
    fun `the bands that produce a usable scan are the three in the middle`() {
        val usable = MotionBand.entries.filter { it.isUsable }

        assertThat(usable).containsExactly(MotionBand.Slow, MotionBand.Ideal, MotionBand.Fast)
    }

    @Test
    fun `the band the guidance warns about is the band the engine rejects frames at`() {
        // Two thresholds for one idea is two thresholds that drift apart: the meter would show
        // a good pace while every frame was being thrown away for blur.
        assertThat(MotionBand.FAST).isEqualTo(GuidanceEngine.MAX_SPEED_METRES_PER_SECOND)
    }

    @Test
    fun `standing still while recording asks the user to move`() {
        val prompt = GuidanceEngine.evaluate(inputs(speed = 0f))

        assertThat(prompt).isEqualTo(GuidancePrompt.StartMoving)
    }

    @Test
    fun `standing still with the viewfinder merely open says nothing about it`() {
        val prompt = GuidanceEngine.evaluate(inputs(speed = 0f, recording = false))

        assertThat(prompt).isNotEqualTo(GuidancePrompt.StartMoving)
    }

    @Test
    fun `nothing being kept for long enough outranks every piece of framing advice`() {
        val prompt = GuidanceEngine.evaluate(
            inputs(millisSinceKeyframe = GuidanceEngine.STALLED_MILLIS + 1),
        )

        assertThat(prompt).isEqualTo(GuidancePrompt.NothingAccepted)
    }

    @Test
    fun `a normal gap between kept frames is not a stall`() {
        val prompt = GuidanceEngine.evaluate(
            inputs(millisSinceKeyframe = GuidanceEngine.STALLED_MILLIS - 1),
        )

        assertThat(prompt).isNotEqualTo(GuidancePrompt.NothingAccepted)
    }

    @Test
    fun `losing tracking still outranks a stall, because it explains it`() {
        val prompt = GuidanceEngine.evaluate(
            inputs(
                millisSinceKeyframe = GuidanceEngine.STALLED_MILLIS + 1,
                tracking = TrackingState.Lost,
            ),
        )

        assertThat(prompt).isEqualTo(GuidancePrompt.HoldStill)
    }
}
