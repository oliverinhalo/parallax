package uk.co.jacoblevy.parallax.feature.capture.logic

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.model.Pose
import kotlin.math.cos
import kotlin.math.sin

class CoverageTrackerTest {

    /** A camera pose at a position, with an identity orientation. */
    private fun poseAt(x: Float, y: Float, z: Float): Pose = Pose(
        floatArrayOf(
            1f, 0f, 0f, 0f,
            0f, 1f, 0f, 0f,
            0f, 0f, 1f, 0f,
            x, y, z, 1f,
        ),
    )

    /** A camera on a circle of [radius] at [height], [degrees] round from +X. */
    private fun orbitPose(degrees: Double, radius: Float = 1f, height: Float = 0f): Pose {
        val radians = Math.toRadians(degrees)
        return poseAt((radius * cos(radians)).toFloat(), height, (radius * sin(radians)).toFloat())
    }

    @Test
    fun `nothing is recorded until a centre is known`() {
        val tracker = CoverageTracker()
        assertThat(tracker.hasCentre).isFalse()
        assertThat(tracker.record(orbitPose(0.0))).isEqualTo(0f)
        assertThat(tracker.coverage).isEqualTo(0f)
        assertThat(tracker.largestGap()).isEqualTo(CoverageGap.None)
    }

    @Test
    fun `a camera at the centre has no direction to record`() {
        val tracker = CoverageTracker().apply { setCentre(0f, 0f, 0f) }
        // Normal for the first frame of a room scan, before the user has moved.
        assertThat(tracker.record(poseAt(0f, 0f, 0f))).isEqualTo(0f)
        assertThat(tracker.coverage).isEqualTo(0f)
    }

    @Test
    fun `a bin fills over several frames rather than on the first sighting`() {
        val tracker = CoverageTracker(framesPerBin = 3).apply { setCentre(0f, 0f, 0f) }
        val pose = orbitPose(0.0)

        // One glancing frame from behind an object is not coverage of its back.
        tracker.record(pose)
        assertThat(tracker.binAt(elevationOf(0f), azimuthOf(0.0))).isWithin(1e-5f).of(1f / 3f)
        tracker.record(pose)
        tracker.record(pose)
        assertThat(tracker.binAt(elevationOf(0f), azimuthOf(0.0))).isEqualTo(1f)
        // And it stops at full rather than accumulating past it.
        assertThat(tracker.record(pose)).isEqualTo(0f)
    }

    @Test
    fun `a full orbit at one height does not count as full coverage`() {
        val tracker = CoverageTracker(framesPerBin = 1).apply { setCentre(0f, 0f, 0f) }
        for (degrees in 0 until 360 step 5) {
            repeat(2) { tracker.record(orbitPose(degrees.toDouble())) }
        }
        // One ring of a sphere is one row of six, so this must not read as "done".
        assertThat(tracker.coverage).isLessThan(0.4f)
        assertThat(tracker.isSufficient()).isFalse()
    }

    @Test
    fun `azimuth wraps so neighbouring directions land in neighbouring bins`() {
        val tracker = CoverageTracker(framesPerBin = 1).apply { setCentre(0f, 0f, 0f) }
        // Two poses a degree apart either side of the +-180 seam.
        tracker.record(orbitPose(179.5))
        tracker.record(orbitPose(-179.5))
        val filled = (0 until 24).filter { tracker.binAt(elevationOf(0f), it) > 0f }
        // Clamping instead of wrapping would put these at opposite ends of the row.
        assertThat(filled.size).isAtMost(2)
        if (filled.size == 2) {
            val separation = minOf(
                filled[1] - filled[0],
                24 - (filled[1] - filled[0]),
            )
            assertThat(separation).isAtMost(1)
        }
    }

    @Test
    fun `an unseen underside is reported before anything else`() {
        val tracker = CoverageTracker(framesPerBin = 1).apply { setCentre(0f, 0f, 0f) }
        // Everything from the equator upwards, nothing from below.
        for (degrees in 0 until 360 step 10) {
            for (height in listOf(0.0f, 0.6f, 1.2f, 2.0f)) {
                repeat(2) { tracker.record(orbitPose(degrees.toDouble(), height = height)) }
            }
        }
        assertThat(tracker.largestGap()).isEqualTo(CoverageGap.Underside)
    }

    @Test
    fun `an unseen top is reported once the underside is covered`() {
        val tracker = CoverageTracker(framesPerBin = 1).apply { setCentre(0f, 0f, 0f) }
        for (degrees in 0 until 360 step 10) {
            for (height in listOf(-2.0f, -1.2f, -0.6f, 0.0f)) {
                repeat(2) { tracker.record(orbitPose(degrees.toDouble(), height = height)) }
            }
        }
        assertThat(tracker.largestGap()).isEqualTo(CoverageGap.Top)
    }

    @Test
    fun `a missing arc opposite the covered side reads as the back`() {
        val tracker = CoverageTracker(framesPerBin = 1).apply { setCentre(0f, 0f, 0f) }
        // Top and bottom rows covered all round, so elevation is not the complaint, but only
        // the front half is covered in the middle.
        for (degrees in 0 until 360 step 10) {
            for (height in listOf(-2.2f, 2.2f)) {
                repeat(2) { tracker.record(orbitPose(degrees.toDouble(), height = height)) }
            }
        }
        for (degrees in -60 until 60 step 5) {
            for (height in listOf(-0.4f, 0f, 0.4f)) {
                repeat(2) { tracker.record(orbitPose(degrees.toDouble(), height = height)) }
            }
        }
        assertThat(tracker.largestGap()).isAnyOf(CoverageGap.Back, CoverageGap.Sides)
    }

    @Test
    fun `a well covered sphere reports no gap and reads as sufficient`() {
        val tracker = CoverageTracker(framesPerBin = 1).apply { setCentre(0f, 0f, 0f) }
        for (degrees in 0 until 360 step 8) {
            for (height in listOf(-2.4f, -0.8f, -0.2f, 0.2f, 0.8f, 2.4f)) {
                repeat(2) { tracker.record(orbitPose(degrees.toDouble(), height = height)) }
            }
        }
        assertThat(tracker.coverage).isGreaterThan(CoverageTracker.SUFFICIENT_COVERAGE)
        assertThat(tracker.isSufficient()).isTrue()
        assertThat(tracker.largestGap()).isEqualTo(CoverageGap.None)
    }

    @Test
    fun `the recorded gain is positive only while a bin is still filling`() {
        val tracker = CoverageTracker(framesPerBin = 2).apply { setCentre(0f, 0f, 0f) }
        val pose = orbitPose(0.0)
        assertThat(tracker.record(pose)).isGreaterThan(0f)
        assertThat(tracker.record(pose)).isGreaterThan(0f)
        assertThat(tracker.record(pose)).isEqualTo(0f)
    }

    @Test
    fun `a reset clears both the bins and the centre`() {
        val tracker = CoverageTracker(framesPerBin = 1).apply { setCentre(0f, 0f, 0f) }
        tracker.record(orbitPose(0.0))
        tracker.reset()
        assertThat(tracker.coverage).isEqualTo(0f)
        assertThat(tracker.hasCentre).isFalse()
    }

    @Test
    fun `the snapshot is a copy, so the UI cannot see a half-written frame`() {
        val tracker = CoverageTracker(framesPerBin = 1).apply { setCentre(0f, 0f, 0f) }
        tracker.record(orbitPose(0.0))
        val snapshot = tracker.snapshot()
        tracker.record(orbitPose(90.0))
        assertThat(snapshot).isNotEqualTo(tracker.snapshot())
    }

    /** The elevation bin a given height maps to on a unit sphere, for the assertions above. */
    private fun elevationOf(height: Float): Int {
        val fraction = (kotlin.math.asin(height.coerceIn(-1f, 1f).toDouble()) + Math.PI / 2) /
            Math.PI
        return kotlin.math.floor(fraction * 6).toInt().coerceIn(0, 5)
    }

    private fun azimuthOf(degrees: Double): Int {
        val fraction = (Math.toRadians(degrees) + Math.PI) / (2 * Math.PI)
        return kotlin.math.floor(fraction * 24).toInt().mod(24)
    }
}
