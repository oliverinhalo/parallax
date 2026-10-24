package uk.co.jacoblevy.parallax.feature.capture.logic

import uk.co.jacoblevy.parallax.core.model.CoverageMap
import uk.co.jacoblevy.parallax.core.model.Pose
import kotlin.math.abs
import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.floor
import kotlin.math.sqrt

/** Which part of the subject the user has not looked at yet. */
enum class CoverageGap {
    None,
    Underside,
    Top,
    Back,
    Sides,
}

/**
 * Tracks which directions the subject has been seen from.
 *
 * The model is a sphere of bins around a centre. Each camera position maps to one bin by its
 * azimuth and elevation relative to that centre, and a bin fills up as frames accumulate rather
 * than flipping on the first sighting: a single glancing frame from behind an object is not
 * coverage of its back, and treating it as such is how a scan ends up with a hole exactly where
 * the user was told it was fine to stop.
 *
 * The centre matters more than the binning. In object mode it is the subject, which the capture
 * screen sets from the first detected plane hit or from the running centroid of the camera path's
 * look-at points; in room and area mode the user is *inside* the sphere, so the centre is their
 * own path and the bins record which way they have looked.
 */
class CoverageTracker(
    private val azimuthBins: Int = CoverageMap.DEFAULT_AZIMUTH_BINS,
    private val elevationBins: Int = CoverageMap.DEFAULT_ELEVATION_BINS,
    /** Frames needed in one bin before it counts as fully covered. */
    private val framesPerBin: Int = DEFAULT_FRAMES_PER_BIN,
) {
    private val bins = FloatArray(azimuthBins * elevationBins)
    private var centreX = 0f
    private var centreY = 0f
    private var centreZ = 0f
    private var haveCentre = false

    /** Set the point the capture is orbiting. */
    fun setCentre(x: Float, y: Float, z: Float) {
        centreX = x
        centreY = y
        centreZ = z
        haveCentre = true
    }

    val hasCentre: Boolean get() = haveCentre

    /**
     * Record a camera position.
     *
     * Returns the fraction this call added, which the capture screen uses to decide whether a
     * frame earned its place: a frame that adds nothing to coverage and nothing to parallax is a
     * frame that costs storage and processing time for no gain.
     */
    fun record(pose: Pose): Float {
        if (!haveCentre) return 0f
        val dx = pose.translationX - centreX
        val dy = pose.translationY - centreY
        val dz = pose.translationZ - centreZ
        val distance = sqrt(dx * dx + dy * dy + dz * dz)
        // A camera at the centre has no direction to record. This is the normal case for the
        // first frame of a room scan before the user has moved.
        if (distance < MIN_RADIUS_METRES) return 0f

        val index = binIndexFor(dx / distance, dy / distance, dz / distance)
        val before = bins[index]
        bins[index] = (before + 1f / framesPerBin).coerceAtMost(1f)
        return (bins[index] - before) / bins.size
    }

    fun snapshot(): CoverageMap = CoverageMap(
        azimuthBins = azimuthBins,
        elevationBins = elevationBins,
        bins = bins.copyOf(),
    )

    val coverage: Float get() = snapshot().coverage

    /** True once enough of the sphere has been seen that stopping is reasonable. */
    fun isSufficient(threshold: Float = SUFFICIENT_COVERAGE): Boolean = coverage >= threshold

    /**
     * The most useful thing the user could do next.
     *
     * Elevation is checked before azimuth. Tops and undersides are the parts people genuinely
     * forget, and they are also the parts that leave a visible hole in the finished model;
     * missing one side of an orbit usually fills in as the user keeps walking.
     */
    fun largestGap(): CoverageGap {
        if (!haveCentre) return CoverageGap.None
        elevationGap()?.let { return it }

        // The emptiest arc of azimuth, measured over the middle rows only.
        //
        // Averaging over every elevation lets the poles mask a missing side: with the top and
        // bottom rows filled all the way round, a column with nothing at all in between still
        // averages a third, which is above any threshold worth setting. The middle rows are
        // also where the user actually walks, so they are what the advice can act on.
        val firstRow = if (elevationBins > 2) 1 else 0
        val lastRow = if (elevationBins > 2) elevationBins - 2 else elevationBins - 1
        val middleRows = lastRow - firstRow + 1
        val columns = FloatArray(azimuthBins) { azimuth ->
            var sum = 0f
            for (elevation in firstRow..lastRow) sum += binAt(elevation, azimuth)
            sum / middleRows
        }
        val emptiest = columns.indices.minByOrNull { columns[it] } ?: return CoverageGap.None
        if (columns[emptiest] >= COLUMN_THRESHOLD) return CoverageGap.None

        // An arc opposite the best-covered direction is "the back"; anything else is a side.
        val fullest = columns.indices.maxByOrNull { columns[it] } ?: return CoverageGap.Sides
        val separation = circularDistance(emptiest, fullest, azimuthBins)
        // An arc at least a quarter-turn away from the best-covered direction is "the back";
        // anything nearer is a side the user can see from where they are standing.
        return if (separation >= azimuthBins / QUARTER_TURN_DIVISOR) {
            CoverageGap.Back
        } else {
            CoverageGap.Sides
        }
    }

    fun binAt(elevation: Int, azimuth: Int): Float {
        if (elevation !in 0 until elevationBins) return 0f
        if (azimuth !in 0 until azimuthBins) return 0f
        return bins[elevation * azimuthBins + azimuth]
    }

    fun reset() {
        bins.fill(0f)
        haveCentre = false
    }

    /** An unseen top or underside, which is what people actually forget. */
    private fun elevationGap(): CoverageGap? = when {
        rowCoverage(0) < ROW_THRESHOLD -> CoverageGap.Underside
        rowCoverage(elevationBins - 1) < ROW_THRESHOLD -> CoverageGap.Top
        else -> null
    }

    private fun rowCoverage(elevation: Int): Float {
        var sum = 0f
        for (azimuth in 0 until azimuthBins) sum += binAt(elevation, azimuth)
        return sum / azimuthBins
    }

    /**
     * Map a unit direction to a bin.
     *
     * Azimuth wraps, so it is taken modulo the bin count rather than clamped: a camera at
     * exactly -180 degrees and one at +180 are the same direction, and clamping would make two
     * neighbouring positions land in bins at opposite ends of the array.
     */
    private fun binIndexFor(x: Float, y: Float, z: Float): Int {
        val azimuthFraction = (atan2(z.toDouble(), x.toDouble()) + Math.PI) / (2.0 * Math.PI)
        val azimuth = floor(azimuthFraction * azimuthBins).toInt().mod(azimuthBins)

        val elevationFraction = (asin(y.coerceIn(-1f, 1f).toDouble()) + Math.PI / 2.0) / Math.PI
        val elevation = floor(elevationFraction * elevationBins).toInt()
            .coerceIn(0, elevationBins - 1)

        return elevation * azimuthBins + azimuth
    }

    private fun circularDistance(a: Int, b: Int, size: Int): Int {
        val direct = abs(a - b)
        return minOf(direct, size - direct)
    }

    companion object {
        const val DEFAULT_FRAMES_PER_BIN = 3
        const val SUFFICIENT_COVERAGE = 0.62f

        /** A camera closer than this to the centre has no meaningful direction. */
        const val MIN_RADIUS_METRES = 0.05f

        private const val QUARTER_TURN_DIVISOR = 4
        private const val ROW_THRESHOLD = 0.25f
        private const val COLUMN_THRESHOLD = 0.3f
    }
}
