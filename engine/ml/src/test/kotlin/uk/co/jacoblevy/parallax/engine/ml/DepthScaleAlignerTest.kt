package uk.co.jacoblevy.parallax.engine.ml

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import kotlin.math.abs
import kotlin.random.Random

/**
 * The scale alignment is the one part of the monocular depth path with real maths in it, and
 * the one part that can be tested without the model: given a synthetic "network output" with a
 * known scale and shift, the fit has to recover it.
 */
class DepthScaleAlignerTest {

    private val width = 64
    private val height = 48

    /**
     * Build a network-like relative-depth frame from true metric depths.
     *
     * The network predicts something affine in *inverse* depth, so that is what is synthesised:
     * `predicted = (1/metres - shift) / scale`, which is the exact inverse of what the aligner
     * has to solve for.
     */
    private fun syntheticFrame(
        trueDepthAt: (Int, Int) -> Float,
        scale: Float,
        shift: Float,
    ): FloatArray {
        val values = FloatArray(width * height)
        for (y in 0 until height) {
            for (x in 0 until width) {
                val metres = trueDepthAt(x, y)
                values[y * width + x] = ((1f / metres) - shift) / scale
            }
        }
        return values
    }

    private fun anchorsFrom(
        trueDepthAt: (Int, Int) -> Float,
        count: Int,
        random: Random = Random(11),
    ): List<DepthAnchor> = (0 until count).map {
        val x = random.nextInt(width)
        val y = random.nextInt(height)
        DepthAnchor(x, y, trueDepthAt(x, y))
    }

    /** A plane receding from 1 m at the top to 3 m at the bottom. */
    private val slope: (Int, Int) -> Float = { _, y -> 1f + 2f * y / height }

    @Test
    fun `it recovers a known scale and shift`() {
        val scale = 0.35f
        val shift = 0.08f
        val frame = syntheticFrame(slope, scale, shift)
        val anchors = anchorsFrom(slope, count = 40)

        val fit = DepthScaleAligner.fit(frame, width, height, anchors)

        assertThat(fit).isNotNull()
        requireNotNull(fit)
        assertThat(fit.isUsable).isTrue()
        assertThat(fit.scale).isWithin(1e-3f).of(scale)
        assertThat(fit.shift).isWithin(1e-3f).of(shift)
        assertThat(fit.medianErrorMetres).isLessThan(0.01f)
    }

    @Test
    fun `the fitted depths match the truth across the frame`() {
        val frame = syntheticFrame(slope, scale = 0.5f, shift = -0.05f)
        val fit = requireNotNull(
            DepthScaleAligner.fit(frame, width, height, anchorsFrom(slope, 30)),
        )

        val metric = DepthScaleAligner.toMetricDepth(frame, width, height, fit)

        var worst = 0f
        for (y in 0 until height) {
            for (x in 0 until width) {
                val expected = slope(x, y)
                worst = maxOf(worst, abs(metric[y * width + x] - expected))
            }
        }
        assertThat(worst).isLessThan(0.02f)
    }

    @Test
    fun `outlying anchors do not drag the fit`() {
        val scale = 0.4f
        val shift = 0.05f
        val frame = syntheticFrame(slope, scale, shift)
        val good = anchorsFrom(slope, count = 40)
        // A quarter of the anchors are mis-triangulated by metres, which is exactly what a
        // wide-baseline outlier looks like. Least squares alone would be ruined by these.
        val poisoned = good.take(10).map { it.copy(metres = it.metres + 4f) } + good.drop(10)

        val fit = requireNotNull(
            DepthScaleAligner.fit(frame, width, height, poisoned),
        )

        assertThat(fit.scale).isWithin(0.02f).of(scale)
        assertThat(fit.shift).isWithin(0.02f).of(shift)
        assertThat(fit.inlierCount).isAtLeast(25)
    }

    @Test
    fun `it refuses to guess from too few anchors`() {
        val frame = syntheticFrame(slope, 0.4f, 0.05f)
        assertThat(DepthScaleAligner.fit(frame, width, height, anchorsFrom(slope, 3))).isNull()
        assertThat(DepthScaleAligner.fit(frame, width, height, emptyList())).isNull()
    }

    @Test
    fun `anchors outside the frame are ignored rather than read out of bounds`() {
        val frame = syntheticFrame(slope, 0.4f, 0.05f)
        val anchors = anchorsFrom(slope, 30) + listOf(
            DepthAnchor(-1, 0, 1f),
            DepthAnchor(0, -1, 1f),
            DepthAnchor(width, 0, 1f),
            DepthAnchor(0, height, 1f),
            DepthAnchor(width * 10, height * 10, 1f),
        )

        val fit = DepthScaleAligner.fit(frame, width, height, anchors)

        assertThat(fit).isNotNull()
    }

    @Test
    fun `implausible anchor depths are discarded`() {
        val frame = syntheticFrame(slope, 0.4f, 0.05f)
        // A point at 2 km and one at a millimetre are both triangulation failures, not scenes.
        val anchors = anchorsFrom(slope, 30) + listOf(
            DepthAnchor(1, 1, 2_000f),
            DepthAnchor(2, 2, 0.0001f),
            DepthAnchor(3, 3, -5f),
        )

        val fit = requireNotNull(DepthScaleAligner.fit(frame, width, height, anchors))
        assertThat(fit.scale).isWithin(0.02f).of(0.4f)
    }

    @Test
    fun `a degenerate frame where every pixel predicts the same value yields nothing`() {
        // No spread in the prediction means no information about the slope.
        val flat = FloatArray(width * height) { 0.5f }
        val anchors = (0 until 20).map { DepthAnchor(it, it, 1f + it * 0.1f) }

        assertThat(DepthScaleAligner.fit(flat, width, height, anchors)).isNull()
    }

    @Test
    fun `a buffer smaller than the stated size is rejected rather than read past`() {
        val short = FloatArray(10)
        val anchors = anchorsFrom(slope, 30)
        assertThat(DepthScaleAligner.fit(short, width, height, anchors)).isNull()
    }

    @Test
    fun `nonsense dimensions are rejected`() {
        val frame = syntheticFrame(slope, 0.4f, 0.05f)
        assertThat(DepthScaleAligner.fit(frame, 0, height, anchorsFrom(slope, 20))).isNull()
        assertThat(DepthScaleAligner.fit(frame, width, -1, anchorsFrom(slope, 20))).isNull()
    }

    @Test
    fun `extrapolated depths outside the plausible range come back unmeasured`() {
        // A fit that maps part of the frame to 200 m or to behind the camera: the network
        // extrapolates confidently past the range its anchors covered, and a confidently wrong
        // depth makes fusion carve a hole through the real surface.
        val fit = DepthScale(scale = 1f, shift = 0f, inlierCount = 20, medianErrorMetres = 0f)
        val relative = floatArrayOf(1f, 0.5f, 0.001f, -1f, 0f)

        val metric = DepthScaleAligner.toMetricDepth(relative, relative.size, 1, fit)

        assertThat(metric[0]).isWithin(1e-5f).of(1f)
        assertThat(metric[1]).isWithin(1e-5f).of(2f)
        // 1/0.001 is a kilometre away; 1/-1 and 1/0 are not depths at all.
        assertThat(metric[2]).isEqualTo(0f)
        assertThat(metric[3]).isEqualTo(0f)
        assertThat(metric[4]).isEqualTo(0f)
    }

    @Test
    fun `the fit is deterministic, so a bad scan can be reproduced`() {
        val frame = syntheticFrame(slope, 0.4f, 0.05f)
        val anchors = anchorsFrom(slope, 40)

        val first = DepthScaleAligner.fit(frame, width, height, anchors)
        val second = DepthScaleAligner.fit(frame, width, height, anchors)

        assertThat(first).isEqualTo(second)
    }

    @Test
    fun `an unusable fit is reported as such rather than returned silently`() {
        val barely = DepthScale(scale = 0.4f, shift = 0f, inlierCount = 3, medianErrorMetres = 0f)
        assertThat(barely.isUsable).isFalse()
        val negative = DepthScale(scale = -1f, shift = 0f, inlierCount = 50, medianErrorMetres = 0f)
        assertThat(negative.isUsable).isFalse()
    }

    @Test
    fun `it works on a real-looking depth distribution`() {
        // Most of a room-scan frame is a wall two to four metres away, with a near object in
        // one corner: the distribution is bimodal, not a smooth ramp.
        val realistic: (Int, Int) -> Float = { x, y ->
            if (x < width / 4 && y > height / 2) 0.6f else 2.5f + (x % 7) * 0.1f
        }
        val frame = syntheticFrame(realistic, scale = 0.3f, shift = 0.02f)
        val anchors = anchorsFrom(realistic, count = 60, random = Random(5))

        val fit = requireNotNull(DepthScaleAligner.fit(frame, width, height, anchors))
        assertThat(fit.medianErrorMetres).isLessThan(0.05f)
    }
}
