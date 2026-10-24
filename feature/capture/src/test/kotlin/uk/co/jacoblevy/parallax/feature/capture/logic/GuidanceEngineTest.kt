package uk.co.jacoblevy.parallax.feature.capture.logic

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.GuidancePrompt
import uk.co.jacoblevy.parallax.core.model.ThermalStatus
import uk.co.jacoblevy.parallax.core.model.TrackingState

class GuidanceEngineTest {

    private val good = FrameQuality(
        sharpness = 150f,
        exposure = 120f,
        contrast = 40f,
        acceptable = true,
    )

    /**
     * A healthy scan in progress.
     *
     * Tests vary one thing with `copy`, which reads better than a twelve-argument factory and
     * makes each test say exactly what it is changing.
     */
    private val healthy = GuidanceInputs(
        mode = CaptureMode.Object,
        trackingState = TrackingState.Tracking,
        quality = good,
        coverage = 0.2f,
        coverageGap = CoverageGap.None,
        speedMetresPerSecond = 0.2f,
        subjectDistanceMetres = 0.8f,
        thermalStatus = ThermalStatus.Nominal,
        batteryPercent = 80,
        storageAvailableBytes = 20L * 1024 * 1024 * 1024,
        keyframeCount = 30,
        recentRejectionRate = 0f,
    )

    @Test
    fun `a healthy scan is encouraged rather than nagged`() {
        assertThat(GuidanceEngine.evaluate(healthy)).isEqualTo(GuidancePrompt.KeepGoing)
    }

    @Test
    fun `a scan with nothing captured yet says nothing at all`() {
        // Before the first keyframe there is no advice to give that is not guesswork.
        assertThat(GuidanceEngine.evaluate(healthy.copy(keyframeCount = 0, coverage = 0f))).isNull()
    }

    @Test
    fun `good coverage invites the user to stop`() {
        val prompt = GuidanceEngine.evaluate(healthy.copy(coverage = 0.8f))
        assertThat(prompt).isEqualTo(GuidancePrompt.ReadyToFinish)
    }

    @Test
    fun `running out of storage outranks everything, including losing tracking`() {
        // Storage is the one condition that destroys the scan already captured.
        val prompt = GuidanceEngine.evaluate(
            healthy.copy(
                storageAvailableBytes = 100L * 1024 * 1024,
                trackingState = TrackingState.Lost,
                thermalStatus = ThermalStatus.Critical,
            ),
        )
        assertThat(prompt).isEqualTo(GuidancePrompt.LowStorage)
    }

    @Test
    fun `lost tracking outranks a framing hint`() {
        val prompt = GuidanceEngine.evaluate(
            healthy.copy(trackingState = TrackingState.Lost, subjectDistanceMetres = 4f),
        )
        assertThat(prompt).isEqualTo(GuidancePrompt.HoldStill)
    }

    @Test
    fun `moving too fast is reported even when the frame happened to be sharp`() {
        val prompt = GuidanceEngine.evaluate(healthy.copy(speedMetresPerSecond = 1.2f))
        assertThat(prompt).isEqualTo(GuidancePrompt.SlowDown)
    }

    @Test
    fun `each frame rejection maps to the advice that fixes it`() {
        val cases = mapOf(
            FrameQuality(sharpness = 10f, exposure = 120f, contrast = 40f) to
                GuidancePrompt.SlowDown,
            FrameQuality(sharpness = 200f, exposure = 5f, contrast = 40f) to
                GuidancePrompt.MoreLight,
            FrameQuality(sharpness = 200f, exposure = 250f, contrast = 40f) to
                GuidancePrompt.TooBright,
            FrameQuality(sharpness = 200f, exposure = 120f, contrast = 2f) to
                GuidancePrompt.LowTexture,
        )
        for ((quality, expected) in cases) {
            assertThat(GuidanceEngine.evaluate(healthy.copy(quality = quality, speedMetresPerSecond = 0.1f)))
                .isEqualTo(expected)
        }
    }

    @Test
    fun `a high rejection rate is reported even when this frame passed`() {
        val prompt = GuidanceEngine.evaluate(healthy.copy(recentRejectionRate = 0.7f, speedMetresPerSecond = 0.1f))
        assertThat(prompt).isEqualTo(GuidancePrompt.SlowDown)
    }

    @Test
    fun `object mode asks the user to close the distance, room mode does not`() {
        assertThat(GuidanceEngine.evaluate(healthy.copy(mode = CaptureMode.Object, subjectDistanceMetres = 5f)))
            .isEqualTo(GuidancePrompt.MoveCloser)
        // In a room, five metres away is simply the far wall.
        assertThat(GuidanceEngine.evaluate(healthy.copy(mode = CaptureMode.Room, subjectDistanceMetres = 5f)))
            .isNotEqualTo(GuidancePrompt.MoveCloser)
    }

    @Test
    fun `being too close to focus is reported in every mode`() {
        for (mode in listOf(CaptureMode.Object, CaptureMode.Room, CaptureMode.Area)) {
            assertThat(GuidanceEngine.evaluate(healthy.copy(mode = mode, subjectDistanceMetres = 0.05f)))
                .isEqualTo(GuidancePrompt.MoveBack)
        }
    }

    @Test
    fun `an unknown distance produces no framing advice`() {
        val prompt = GuidanceEngine.evaluate(healthy.copy(subjectDistanceMetres = null))
        assertThat(prompt).isEqualTo(GuidancePrompt.KeepGoing)
    }

    @Test
    fun `coverage gaps become specific instructions`() {
        val cases = mapOf(
            CoverageGap.Underside to GuidancePrompt.CaptureLower,
            CoverageGap.Top to GuidancePrompt.CaptureHigher,
            CoverageGap.Back to GuidancePrompt.MissedTheBack,
        )
        for ((gap, expected) in cases) {
            assertThat(GuidanceEngine.evaluate(healthy.copy(coverageGap = gap))).isEqualTo(expected)
        }
    }

    @Test
    fun `coverage advice waits until there is enough of a scan to judge`() {
        // With four frames, "you missed the back" is true of every scan and helps nobody.
        val prompt = GuidanceEngine.evaluate(healthy.copy(keyframeCount = 4, coverageGap = CoverageGap.Back))
        assertThat(prompt).isEqualTo(GuidancePrompt.KeepGoing)
    }

    @Test
    fun `thermal and battery warnings are raised before they end the session`() {
        assertThat(GuidanceEngine.evaluate(healthy.copy(thermalStatus = ThermalStatus.Serious)))
            .isEqualTo(GuidancePrompt.Overheating)
        assertThat(GuidanceEngine.evaluate(healthy.copy(batteryPercent = 8)))
            .isEqualTo(GuidancePrompt.LowBattery)
    }

    @Test
    fun `a battery reading of zero is treated as unknown rather than critical`() {
        // Some devices report 0 before the first battery broadcast arrives, and a "battery low"
        // prompt on a fully charged phone destroys trust in every other prompt.
        assertThat(GuidanceEngine.evaluate(healthy.copy(batteryPercent = 0)))
            .isNotEqualTo(GuidancePrompt.LowBattery)
    }

    @Test
    fun `frames are recorded only while everything needed is true`() {
        assertThat(GuidanceEngine.shouldRecordFrame(healthy)).isTrue()
        assertThat(GuidanceEngine.shouldRecordFrame(healthy.copy(trackingState = TrackingState.Limited)))
            .isFalse()
        assertThat(
            GuidanceEngine.shouldRecordFrame(
                healthy.copy(quality = FrameQuality(sharpness = 1f, acceptable = false)),
            ),
        ).isFalse()
        assertThat(GuidanceEngine.shouldRecordFrame(healthy.copy(thermalStatus = ThermalStatus.Emergency)))
            .isFalse()
        assertThat(GuidanceEngine.shouldRecordFrame(healthy.copy(storageAvailableBytes = 1_024)))
            .isFalse()
    }
}

class GuidanceSessionTest {

    private val session = GuidanceSession(minVisibleMillis = 1_000)

    private fun inputs(
        coverage: Float = 0.2f,
        storage: Long = 20L * 1024 * 1024 * 1024,
        keyframes: Int = 30,
    ) = GuidanceInputs(
        mode = CaptureMode.Object,
        quality = FrameQuality(sharpness = 150f, exposure = 120f, contrast = 40f, acceptable = true),
        coverage = coverage,
        storageAvailableBytes = storage,
        keyframeCount = keyframes,
        subjectDistanceMetres = 0.8f,
    )

    @Test
    fun `the first prompt appears immediately`() {
        assertThat(session.update(inputs(), nowMillis = 0)).isTrue()
        assertThat(session.prompt).isEqualTo(GuidancePrompt.KeepGoing)
    }

    @Test
    fun `an unchanged prompt is not reported as a change`() {
        session.update(inputs(), nowMillis = 0)
        // Otherwise a haptic would fire on every frame for the whole scan.
        assertThat(session.update(inputs(), nowMillis = 500)).isFalse()
    }

    @Test
    fun `a lower priority prompt cannot interrupt one that is still readable`() {
        session.update(inputs(), nowMillis = 0)
        assertThat(session.prompt).isEqualTo(GuidancePrompt.KeepGoing)

        // ReadyToFinish outranks KeepGoing, so it does replace it...
        assertThat(session.update(inputs(coverage = 0.9f), nowMillis = 100)).isTrue()
        // ...but KeepGoing cannot replace ReadyToFinish until the hold expires.
        assertThat(session.update(inputs(coverage = 0.2f), nowMillis = 300)).isFalse()
        assertThat(session.prompt).isEqualTo(GuidancePrompt.ReadyToFinish)

        assertThat(session.update(inputs(coverage = 0.2f), nowMillis = 1_500)).isTrue()
        assertThat(session.prompt).isEqualTo(GuidancePrompt.KeepGoing)
    }

    @Test
    fun `an urgent prompt interrupts immediately`() {
        session.update(inputs(), nowMillis = 0)
        // "Storage nearly full" must not wait behind "looking good".
        assertThat(session.update(inputs(storage = 1_024), nowMillis = 10)).isTrue()
        assertThat(session.prompt).isEqualTo(GuidancePrompt.LowStorage)
    }

    @Test
    fun `clearing the condition clears the prompt once the hold expires`() {
        session.update(inputs(storage = 1_024), nowMillis = 0)
        session.update(inputs(keyframes = 0), nowMillis = 2_000)
        assertThat(session.prompt).isNull()
    }

    @Test
    fun `a reset forgets the visible prompt`() {
        session.update(inputs(), nowMillis = 0)
        session.reset()
        assertThat(session.prompt).isNull()
        assertThat(session.update(inputs(), nowMillis = 1)).isTrue()
    }
}
