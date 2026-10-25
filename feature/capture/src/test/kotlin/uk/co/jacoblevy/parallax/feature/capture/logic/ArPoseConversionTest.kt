package uk.co.jacoblevy.parallax.feature.capture.logic

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.model.Pose

/**
 * The half turn between ARCore's camera and the engine's.
 *
 * The test that matters is the last one: a point the user was pointing the phone at has to end
 * up in front of the engine's camera. Three scans fused twenty-eight frames into an empty
 * volume because it ended up behind.
 */
class ArPoseConversionTest {

    /** Column-major, so element (row, column) is at `column * 4 + row`. */
    private fun at(m: FloatArray, row: Int, column: Int) = m[column * 4 + row]

    private fun identity() = floatArrayOf(
        1f, 0f, 0f, 0f,
        0f, 1f, 0f, 0f,
        0f, 0f, 1f, 0f,
        0f, 0f, 0f, 1f,
    )

    /**
     * Where a world point sits in the engine's camera frame.
     *
     * The engine inverts world-from-camera internally; for a rigid transform that is
     * `Rᵀ (p − t)`, and the sign of the Z it produces is the whole question.
     */
    private fun intoCameraSpace(worldFromCamera: FloatArray, point: Triple<Float, Float, Float>):
        Triple<Float, Float, Float> {
        val tx = at(worldFromCamera, 0, 3)
        val ty = at(worldFromCamera, 1, 3)
        val tz = at(worldFromCamera, 2, 3)
        val dx = point.first - tx
        val dy = point.second - ty
        val dz = point.third - tz
        fun axis(column: Int) =
            at(worldFromCamera, 0, column) * dx +
                at(worldFromCamera, 1, column) * dy +
                at(worldFromCamera, 2, column) * dz
        return Triple(axis(0), axis(1), axis(2))
    }

    @Test
    fun `the camera's position is untouched`() {
        val arCore = identity().also {
            it[12] = 1.5f
            it[13] = 1.6f
            it[14] = -2.5f
        }

        val engine = ArPoseConversion.toEngineConvention(arCore)

        assertThat(at(engine, 0, 3)).isEqualTo(1.5f)
        assertThat(at(engine, 1, 3)).isEqualTo(1.6f)
        assertThat(at(engine, 2, 3)).isEqualTo(-2.5f)
    }

    @Test
    fun `up becomes down and back becomes forward`() {
        val engine = ArPoseConversion.toEngineConvention(identity())

        // ARCore's +Y is up in the world; the engine's +Y is down, so it points the other way.
        assertThat(at(engine, 1, 1)).isEqualTo(-1f)
        // ARCore's camera +Z points backwards out of the screen; the engine's points forwards.
        assertThat(at(engine, 2, 2)).isEqualTo(-1f)
        // And right stays right, or the handedness would be wrong and the model mirrored.
        assertThat(at(engine, 0, 0)).isEqualTo(1f)
    }

    @Test
    fun `the conversion is its own inverse`() {
        val arCore = identity()

        val roundTrip = ArPoseConversion.toEngineConvention(
            ArPoseConversion.toEngineConvention(arCore),
        )

        assertThat(roundTrip).isEqualTo(arCore)
    }

    @Test
    fun `a matrix of the wrong size is rejected rather than read past`() {
        runCatching { ArPoseConversion.toEngineConvention(FloatArray(9)) }
            .let { assertThat(it.isFailure).isTrue() }
    }

    @Test
    fun `what the phone was pointed at ends up in front of the engine's camera`() {
        // ARCore, identity rotation, camera at the origin: the phone is pointing along world
        // -Z. So a thing being scanned a metre away is at (0, 0, -1).
        val subject = Triple(0f, 0f, -1f)

        val unconverted = intoCameraSpace(identity(), subject)
        val converted = intoCameraSpace(
            ArPoseConversion.toEngineConvention(identity()),
            subject,
        )

        // This is the bug, in one assertion: a metre behind the camera, so it projects nowhere
        // and no voxel is ever observed.
        assertThat(unconverted.third).isLessThan(0f)
        // And this is the fix: a metre in front, where the depth map says it is.
        assertThat(converted.third).isWithin(TOLERANCE).of(1f)
        assertThat(converted.first).isWithin(TOLERANCE).of(0f)
        assertThat(converted.second).isWithin(TOLERANCE).of(0f)
    }

    @Test
    fun `the matrix stays the size the engine requires`() {
        val engine = ArPoseConversion.toEngineConvention(identity())

        assertThat(engine).hasLength(Pose.MATRIX_ELEMENTS)
    }

    private companion object {
        const val TOLERANCE = 1e-5f
    }
}
