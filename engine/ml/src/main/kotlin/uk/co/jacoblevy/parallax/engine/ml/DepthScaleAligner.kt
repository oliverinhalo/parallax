package uk.co.jacoblevy.parallax.engine.ml

import kotlin.math.abs

/** One sparse measurement: a pixel, and the metric depth structure-from-motion found there. */
data class DepthAnchor(val x: Int, val y: Int, val metres: Float)

/** The affine fit that turns a network's relative depth into metres. */
data class DepthScale(
    /** Multiplies the network's *inverse* depth. */
    val scale: Float,
    /** Added to the scaled inverse depth. */
    val shift: Float,
    val inlierCount: Int,
    val medianErrorMetres: Float,
) {
    val isUsable: Boolean get() = scale > 0f && inlierCount >= MIN_INLIERS

    /** Convert one of the network's relative values to metres. */
    fun toMetres(relative: Float): Float {
        val inverse = scale * relative + shift
        return if (inverse > MIN_INVERSE_DEPTH) 1f / inverse else 0f
    }

    companion object {
        const val MIN_INLIERS = 8

        /** An inverse depth below this is 200 m away, i.e. nothing this app reconstructs. */
        const val MIN_INVERSE_DEPTH = 0.005f
    }
}

/**
 * Fits a monocular depth network's output to metric scale.
 *
 * A relative-depth network returns a value per pixel that is monotonic in depth but has no
 * units, no origin, and a different arbitrary range for every frame. On its own it is useless
 * for fusion: integrating two frames whose depths differ by an unknown factor produces two
 * surfaces where there is one.
 *
 * The fix is the sparse points structure-from-motion has already triangulated. They are few --
 * a few hundred per frame at best -- but they are metric, so the network's output can be fitted
 * to them. The fit is affine **in inverse depth**, which is the right space for two reasons:
 * these networks are trained to predict disparity rather than depth, and a projective
 * reconstruction's ambiguity is exactly a scale and shift of inverse depth.
 *
 * RANSAC rather than plain least squares, because the anchors contain outliers: a
 * mis-triangulated point is metres wrong, and one of those drags a least-squares fit far enough
 * to ruin the whole frame.
 */
object DepthScaleAligner {

    /**
     * @param relative the network's output, row-major, [width] by [height].
     * @param anchors sparse metric depths at known pixels.
     */
    fun fit(
        relative: FloatArray,
        width: Int,
        height: Int,
        anchors: List<DepthAnchor>,
        iterations: Int = DEFAULT_ITERATIONS,
        inlierToleranceMetres: Float = DEFAULT_TOLERANCE_METRES,
        seed: Long = DEFAULT_SEED,
    ): DepthScale? {
        if (width <= 0 || height <= 0) return null
        if (relative.size < width * height) return null

        // Pair each anchor with the network's value at its pixel, dropping the ones that are
        // out of frame or at an implausible depth.
        val samples = anchors.mapNotNull { anchor ->
            if (anchor.x !in 0 until width || anchor.y !in 0 until height) return@mapNotNull null
            if (anchor.metres <= MIN_PLAUSIBLE_METRES) return@mapNotNull null
            if (anchor.metres > MAX_PLAUSIBLE_METRES) return@mapNotNull null
            Sample(
                predicted = relative[anchor.y * width + anchor.x],
                inverseMetric = 1f / anchor.metres,
                metres = anchor.metres,
            )
        }
        if (samples.size < DepthScale.MIN_INLIERS) return null

        // A deterministic generator, so a reconstruction is reproducible: debugging a bad scan
        // that cannot be reproduced is close to impossible.
        var random = seed
        fun nextIndex(bound: Int): Int {
            random = random * MULTIPLIER + INCREMENT
            return (((random ushr SHIFT).toInt() and Int.MAX_VALUE) % bound)
        }

        var best: DepthScale? = null
        var bestInliers = 0
        repeat(iterations) {
            val a = samples[nextIndex(samples.size)]
            val b = samples[nextIndex(samples.size)]
            val candidate = solveTwoPoint(a, b) ?: return@repeat

            var inliers = 0
            for (sample in samples) {
                if (abs(candidate.toMetres(sample.predicted) - sample.metres) <=
                    inlierToleranceMetres
                ) {
                    inliers++
                }
            }
            if (inliers > bestInliers) {
                bestInliers = inliers
                best = candidate
            }
        }

        val consensus = best ?: return null
        // Refit on the inliers. The two-point estimate only has to find the right consensus
        // set; the fit that is actually used should see all of it.
        val inliers = samples.filter {
            abs(consensus.toMetres(it.predicted) - it.metres) <= inlierToleranceMetres
        }
        if (inliers.size < DepthScale.MIN_INLIERS) return null
        return leastSquares(inliers)
    }

    private data class Sample(val predicted: Float, val inverseMetric: Float, val metres: Float)

    /**
     * The line through two samples in (predicted, inverse metric depth) space.
     *
     * Two points is the minimal sample for an affine fit, which is what keeps the RANSAC
     * iteration count low enough to run per frame.
     */
    private fun solveTwoPoint(a: Sample, b: Sample): DepthScale? {
        val spread = b.predicted - a.predicted
        // Two samples at the same predicted value say nothing about the slope.
        if (abs(spread) < MIN_PREDICTED_SPREAD) return null
        val scale = (b.inverseMetric - a.inverseMetric) / spread
        // A negative slope would mean the network has depth inverted, which no trained model
        // does; it is always a bad pair of anchors.
        if (scale <= 0f) return null
        val shift = a.inverseMetric - scale * a.predicted
        return DepthScale(scale = scale, shift = shift, inlierCount = 2, medianErrorMetres = 0f)
    }

    /** Ordinary least squares of inverse metric depth against the network's value. */
    private fun leastSquares(samples: List<Sample>): DepthScale? {
        var sumX = 0.0
        var sumY = 0.0
        var sumXy = 0.0
        var sumXx = 0.0
        for (sample in samples) {
            val x = sample.predicted.toDouble()
            val y = sample.inverseMetric.toDouble()
            sumX += x
            sumY += y
            sumXy += x * y
            sumXx += x * x
        }
        val n = samples.size
        val denominator = n * sumXx - sumX * sumX
        // A denominator this small means every sample predicted the same value, so there is
        // no slope to fit.
        if (abs(denominator) < MIN_FIT_DENOMINATOR) return null
        val scale = (n * sumXy - sumX * sumY) / denominator
        if (scale <= 0.0) return null
        val shift = (sumY - scale * sumX) / n

        val fitted = DepthScale(
            scale = scale.toFloat(),
            shift = shift.toFloat(),
            inlierCount = n,
            medianErrorMetres = 0f,
        )
        val errors = samples
            .map { abs(fitted.toMetres(it.predicted) - it.metres) }
            .sorted()
        return fitted.copy(medianErrorMetres = errors[errors.size / 2])
    }

    /**
     * Apply a fit to a whole frame, producing metric depth.
     *
     * Values the fit maps outside the plausible range come back as 0, which every consumer
     * reads as "no measurement". That is deliberate: the network extrapolates confidently past
     * the range its anchors covered, and a confidently wrong depth makes fusion carve a hole
     * through the real surface.
     */
    fun toMetricDepth(
        relative: FloatArray,
        width: Int,
        height: Int,
        fit: DepthScale,
        maxDepthMetres: Float = MAX_PLAUSIBLE_METRES,
    ): FloatArray {
        val out = FloatArray(width * height)
        for (i in out.indices) {
            val metres = fit.toMetres(relative[i])
            out[i] = if (metres in MIN_PLAUSIBLE_METRES..maxDepthMetres) metres else 0f
        }
        return out
    }

    const val MIN_PLAUSIBLE_METRES = 0.05f
    const val MAX_PLAUSIBLE_METRES = 30f

    private const val DEFAULT_ITERATIONS = 200
    private const val DEFAULT_TOLERANCE_METRES = 0.08f
    private const val DEFAULT_SEED = 0x5DEECE66DL
    private const val MIN_PREDICTED_SPREAD = 1e-4f
    private const val MIN_FIT_DENOMINATOR = 1e-12

    // A 48-bit linear congruential generator, as java.util.Random uses. Deterministic across
    // platforms, which `Random` with a seed is not guaranteed to be across Kotlin targets.
    private const val MULTIPLIER = 0x5DEECE66DL
    private const val INCREMENT = 0xBL
    private const val SHIFT = 16
}
