package uk.co.jacoblevy.parallax.feature.capture.logic

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.FrameRejection
import uk.co.jacoblevy.parallax.core.model.Pose
import kotlin.math.cos
import kotlin.math.sin

class KeyframeSelectorTest {

    private val good = FrameQuality(
        sharpness = 150f,
        exposure = 120f,
        contrast = 40f,
        acceptable = true,
    )

    private fun poseAt(x: Float, y: Float = 0f, z: Float = 0f): Pose = Pose(
        floatArrayOf(
            1f, 0f, 0f, 0f,
            0f, 1f, 0f, 0f,
            0f, 0f, 1f, 0f,
            x, y, z, 1f,
        ),
    )

    /** A pose rotated [degrees] about the Y axis, at the origin. */
    private fun yawPose(degrees: Double): Pose {
        val c = cos(Math.toRadians(degrees)).toFloat()
        val s = sin(Math.toRadians(degrees)).toFloat()
        return Pose(
            floatArrayOf(
                c, 0f, -s, 0f,
                0f, 1f, 0f, 0f,
                s, 0f, c, 0f,
                0f, 0f, 0f, 1f,
            ),
        )
    }

    private fun KeyframeSelector.offerFrame(
        pose: Pose?,
        quality: FrameQuality = good,
        hash: Long = 0,
        hashDistance: Int = 0,
        coverageGain: Float = 0f,
    ) = offer(pose, quality, hash, hashDistance, coverageGain)

    @Test
    fun `the first acceptable frame is always kept`() {
        val selector = KeyframeSelector()
        val decision = selector.offerFrame(poseAt(0f))
        assertThat(decision).isEqualTo(KeyframeDecision.Keep(KeepReason.First, 0f))
        assertThat(selector.keptCount).isEqualTo(1)
    }

    @Test
    fun `a frame that fails quality is dropped with the reason`() {
        val selector = KeyframeSelector()
        val blurred = FrameQuality(sharpness = 5f, exposure = 120f, contrast = 40f)
        val decision = selector.offerFrame(poseAt(0f), quality = blurred)
        assertThat(decision).isEqualTo(KeyframeDecision.Drop(FrameRejection.Blurred))
        assertThat(selector.keptCount).isEqualTo(0)
    }

    @Test
    fun `standing still produces one keyframe, not thirty`() {
        val selector = KeyframeSelector()
        selector.offerFrame(poseAt(0f))
        repeat(30) {
            assertThat(selector.offerFrame(poseAt(0.001f)).kept).isFalse()
        }
        // Thirty frames from one spot reconstruct worse than ten from ten positions, and cost
        // three times the storage while doing it.
        assertThat(selector.keptCount).isEqualTo(1)
    }

    @Test
    fun `moving far enough earns a keyframe`() {
        val selector = KeyframeSelector(minTranslationMetres = 0.04f)
        selector.offerFrame(poseAt(0f))
        assertThat(selector.offerFrame(poseAt(0.03f)).kept).isFalse()
        val decision = selector.offerFrame(poseAt(0.05f))
        assertThat(decision).isInstanceOf(KeyframeDecision.Keep::class.java)
        assertThat((decision as KeyframeDecision.Keep).reason).isEqualTo(KeepReason.Parallax)
    }

    @Test
    fun `turning on the spot earns a keyframe`() {
        val selector = KeyframeSelector(minRotationDegrees = 6f)
        selector.offerFrame(yawPose(0.0))
        assertThat(selector.offerFrame(yawPose(3.0)).kept).isFalse()
        val decision = selector.offerFrame(yawPose(10.0))
        assertThat((decision as KeyframeDecision.Keep).reason).isEqualTo(KeepReason.Rotation)
    }

    @Test
    fun `new coverage earns a keyframe even without new parallax`() {
        val selector = KeyframeSelector()
        selector.offerFrame(poseAt(0f))
        val decision = selector.offerFrame(poseAt(0.001f), coverageGain = 0.01f)
        assertThat((decision as KeyframeDecision.Keep).reason).isEqualTo(KeepReason.Coverage)
        assertThat(decision.coverageGain).isEqualTo(0.01f)
    }

    @Test
    fun `a clearly sharper frame replaces the blurrier one rather than joining it`() {
        val selector = KeyframeSelector()
        selector.offerFrame(poseAt(0f), quality = good.copy(sharpness = 100f))
        val decision = selector.offerFrame(
            poseAt(0.001f),
            quality = good.copy(sharpness = 200f),
        )
        assertThat((decision as KeyframeDecision.Keep).reason).isEqualTo(KeepReason.Sharper)
        // Replaced, not appended: the viewpoint is the same, so two frames would be waste.
        assertThat(selector.keptCount).isEqualTo(1)
    }

    @Test
    fun `a marginally sharper frame from the same place is dropped`() {
        val selector = KeyframeSelector()
        selector.offerFrame(poseAt(0f), quality = good.copy(sharpness = 100f))
        val decision = selector.offerFrame(
            poseAt(0.001f),
            quality = good.copy(sharpness = 110f),
        )
        assertThat(decision).isEqualTo(KeyframeDecision.Drop(FrameRejection.Duplicate))
    }

    @Test
    fun `the replacement becomes the reference for the next comparison`() {
        val selector = KeyframeSelector(minTranslationMetres = 0.04f)
        selector.offerFrame(poseAt(0f), quality = good.copy(sharpness = 100f))
        selector.offerFrame(poseAt(0.01f), quality = good.copy(sharpness = 300f))
        // Measured from 0.01, not from 0: 0.03 is a 2 cm baseline, below the threshold.
        assertThat(selector.offerFrame(poseAt(0.03f)).kept).isFalse()
        assertThat(selector.offerFrame(poseAt(0.06f)).kept).isTrue()
    }

    @Test
    fun `with no poses at all, image novelty decides`() {
        // The import-from-video path: there is nothing but pixels to go on.
        val selector = KeyframeSelector(minHashDistance = 8)
        selector.offerFrame(pose = null, hash = 0b1010)
        assertThat(selector.offerFrame(pose = null, hashDistance = 2).kept).isFalse()
        assertThat(selector.offerFrame(pose = null, hashDistance = 20).kept).isTrue()
    }

    @Test
    fun `a reset clears the history`() {
        val selector = KeyframeSelector()
        selector.offerFrame(poseAt(0f))
        selector.offerFrame(poseAt(1f))
        selector.reset()
        assertThat(selector.keptCount).isEqualTo(0)
        assertThat((selector.offerFrame(poseAt(0f)) as KeyframeDecision.Keep).reason)
            .isEqualTo(KeepReason.First)
    }

    @Test
    fun `an orbit of a small object still yields plenty of keyframes`() {
        // The 4 cm translation floor is generous for a coin, so rotation and coverage have to
        // carry it: a 30 cm orbit should not collapse to two frames.
        val selector = KeyframeSelector()
        val radius = 0.3
        var kept = 0
        for (degrees in 0 until 360 step 4) {
            val radians = Math.toRadians(degrees.toDouble())
            val pose = Pose(
                floatArrayOf(
                    cos(radians).toFloat(), 0f, -sin(radians).toFloat(), 0f,
                    0f, 1f, 0f, 0f,
                    sin(radians).toFloat(), 0f, cos(radians).toFloat(), 0f,
                    (radius * cos(radians)).toFloat(), 0f, (radius * sin(radians)).toFloat(), 1f,
                ),
            )
            if (selector.offerFrame(pose).kept) kept++
        }
        assertThat(kept).isAtLeast(40)
    }
}

class PoseMathTest {

    private fun yawPose(degrees: Double): Pose {
        val c = cos(Math.toRadians(degrees)).toFloat()
        val s = sin(Math.toRadians(degrees)).toFloat()
        return Pose(
            floatArrayOf(
                c, 0f, -s, 0f,
                0f, 1f, 0f, 0f,
                s, 0f, c, 0f,
                0f, 0f, 0f, 1f,
            ),
        )
    }

    @Test
    fun `the angle between identical orientations is zero, not NaN`() {
        // Floating-point error in the product puts the cosine a hair outside [-1, 1] here, which
        // would make acos return NaN exactly when the camera is barely moving.
        val angle = rotationDegreesBetween(Pose.identity(), Pose.identity())
        assertThat(angle).isNotNaN()
        assertThat(angle).isWithin(1e-3f).of(0f)
    }

    @Test
    fun `known yaw differences are recovered`() {
        for (degrees in listOf(1.0, 5.0, 30.0, 90.0, 179.0)) {
            val angle = rotationDegreesBetween(yawPose(0.0), yawPose(degrees))
            assertThat(angle).isWithin(TOLERANCE_DEGREES).of(degrees.toFloat())
        }
    }

    @Test
    fun `the angle is symmetric and never negative`() {
        val forwards = rotationDegreesBetween(yawPose(10.0), yawPose(70.0))
        val backwards = rotationDegreesBetween(yawPose(70.0), yawPose(10.0))
        assertThat(forwards).isWithin(TOLERANCE_DEGREES).of(backwards)
        assertThat(forwards).isGreaterThan(0f)
    }

    @Test
    fun `speed is metres per second and a zero interval is not a division by zero`() {
        val a = Pose(Pose.identity().matrix.copyOf())
        val b = Pose(Pose.identity().matrix.copyOf().also { it[12] = 2f })
        assertThat(speedBetween(a, b, deltaNanos = 1_000_000_000L)).isWithin(1e-4f).of(2f)
        assertThat(speedBetween(a, b, deltaNanos = 500_000_000L)).isWithin(1e-4f).of(4f)
        assertThat(speedBetween(a, b, deltaNanos = 0)).isEqualTo(0f)
        assertThat(speedBetween(a, b, deltaNanos = -5)).isEqualTo(0f)
    }

    private companion object {
        const val TOLERANCE_DEGREES = 0.05f
    }
}
