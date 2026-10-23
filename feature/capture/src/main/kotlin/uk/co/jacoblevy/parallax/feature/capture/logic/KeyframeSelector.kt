package uk.co.jacoblevy.parallax.feature.capture.logic

import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.FrameRejection
import uk.co.jacoblevy.parallax.core.model.Pose
import kotlin.math.acos

/** Why a candidate frame was kept or dropped. */
sealed interface KeyframeDecision {
    data class Keep(val reason: KeepReason, val coverageGain: Float) : KeyframeDecision
    data class Drop(val reason: FrameRejection) : KeyframeDecision

    val kept: Boolean get() = this is Keep
}

enum class KeepReason {
    /** The first frame, which is always kept: there is nothing to compare it to. */
    First,

    /** Moved far enough to give the geometry a new viewpoint. */
    Parallax,

    /** Looked somewhere new, even without moving much. */
    Rotation,

    /** Filled in part of the subject that had not been seen. */
    Coverage,

    /** Sharper than a recent frame from nearly the same place, so it replaces it. */
    Sharper,
}

/**
 * Decides which frames become keyframes.
 *
 * Reconstruction quality depends on the *baseline* between frames, not on their number. Thirty
 * frames taken from one spot reconstruct worse than ten taken from ten positions, and they cost
 * three times the storage and processing time while doing it. So a frame earns its place by
 * adding one of: a new viewpoint, a new direction, new coverage, or a sharper look at something
 * already seen.
 *
 * The perceptual hash is the backstop. Pose deltas say where the camera was; the hash says
 * whether the image actually changed, which catches the case the poses cannot -- a phone resting
 * on a table with ARCore drifting slowly enough to look like motion.
 */
class KeyframeSelector(
    private val minTranslationMetres: Float = DEFAULT_MIN_TRANSLATION,
    private val minRotationDegrees: Float = DEFAULT_MIN_ROTATION,
    private val minHashDistance: Int = DEFAULT_MIN_HASH_DISTANCE,
) {
    private val kept = mutableListOf<Candidate>()

    private data class Candidate(val pose: Pose?, val hash: Long, val sharpness: Float)

    val keptCount: Int get() = kept.size

    /**
     * @param coverageGain what [CoverageTracker] says this pose would add, 0 when unknown.
     * @param hashDistance Hamming distance to the nearest kept frame's hash; pass
     *   [Int.MAX_VALUE] when no hash is available, which treats the image as definitely new.
     */
    @Suppress("ReturnCount")
    fun offer(
        pose: Pose?,
        quality: FrameQuality,
        imageHash: Long,
        hashDistance: Int,
        coverageGain: Float,
    ): KeyframeDecision {
        quality.rejectionReason?.let { return KeyframeDecision.Drop(it) }

        val previous = kept.lastOrNull()
            ?: return keep(pose, imageHash, quality.sharpness, KeepReason.First, coverageGain)

        // No pose at all: fall back to image novelty alone. This is the import-from-video path,
        // where there is nothing but pixels to go on.
        if (pose == null || previous.pose == null) {
            return if (hashDistance >= minHashDistance) {
                keep(pose, imageHash, quality.sharpness, KeepReason.Parallax, coverageGain)
            } else {
                KeyframeDecision.Drop(FrameRejection.Duplicate)
            }
        }

        val translation = pose.distanceTo(previous.pose)
        val rotation = rotationDegreesBetween(pose, previous.pose)

        if (coverageGain > MIN_COVERAGE_GAIN) {
            return keep(pose, imageHash, quality.sharpness, KeepReason.Coverage, coverageGain)
        }
        if (translation >= minTranslationMetres) {
            return keep(pose, imageHash, quality.sharpness, KeepReason.Parallax, coverageGain)
        }
        if (rotation >= minRotationDegrees) {
            return keep(pose, imageHash, quality.sharpness, KeepReason.Rotation, coverageGain)
        }

        // Nearly the same viewpoint. Worth keeping only if it is clearly sharper, in which case
        // it replaces the blurrier one rather than joining it.
        if (quality.sharpness > previous.sharpness * SHARPNESS_IMPROVEMENT) {
            kept[kept.lastIndex] = Candidate(pose, imageHash, quality.sharpness)
            return KeyframeDecision.Keep(KeepReason.Sharper, coverageGain)
        }
        return KeyframeDecision.Drop(FrameRejection.Duplicate)
    }

    fun reset() = kept.clear()

    private fun keep(
        pose: Pose?,
        hash: Long,
        sharpness: Float,
        reason: KeepReason,
        coverageGain: Float,
    ): KeyframeDecision {
        kept += Candidate(pose, hash, sharpness)
        return KeyframeDecision.Keep(reason, coverageGain)
    }

    companion object {
        /**
         * Minimum baseline between keyframes.
         *
         * 4 cm is small for a room and generous for a coin. It is the floor rather than the
         * target: coverage and rotation also admit frames, so an orbit of a small object still
         * produces plenty of keyframes.
         */
        const val DEFAULT_MIN_TRANSLATION = 0.04f
        const val DEFAULT_MIN_ROTATION = 6f

        /**
         * Hamming distance over a 64-bit perceptual hash.
         *
         * Eight differing bits is about the point where two frames of the same scene are
         * distinguishable rather than noise-apart.
         */
        const val DEFAULT_MIN_HASH_DISTANCE = 8

        const val MIN_COVERAGE_GAIN = 0.0005f
        const val SHARPNESS_IMPROVEMENT = 1.25f
    }
}

/**
 * The angle between two camera orientations, in degrees.
 *
 * Taken from the trace of the relative rotation: for R = Ra^T * Rb, `trace(R) = 1 + 2cos(theta)`,
 * so theta follows from the trace without needing the axis. The rotation blocks are read out of
 * the column-major 4x4s, and the argument is clamped before `acos` because floating-point error
 * in the product puts it a hair outside [-1, 1] for small angles, which would give NaN exactly
 * when the camera is barely moving.
 */
internal fun rotationDegreesBetween(a: Pose, b: Pose): Float {
    // Column-major: element (row, col) sits at col * 4 + row, so column i is
    // matrix[i * 4 .. i * 4 + 2]. The diagonal of Ra^T * Rb is exactly the three
    // column-with-column dot products, which is all the trace needs.
    var trace = 0.0
    for (column in 0 until ROTATION_DIMENSION) {
        for (row in 0 until ROTATION_DIMENSION) {
            val index = column * MATRIX_COLUMN_STRIDE + row
            trace += a.matrix[index].toDouble() * b.matrix[index]
        }
    }
    // trace(R) = 1 + 2 cos(theta), so cos(theta) = (trace - 1) / 2.
    val cosTheta = ((trace - 1.0) / 2.0).coerceIn(-1.0, 1.0)
    return Math.toDegrees(acos(cosTheta)).toFloat()
}

/** Camera speed from two poses and the time between them, in metres per second. */
internal fun speedBetween(a: Pose, b: Pose, deltaNanos: Long): Float {
    if (deltaNanos <= 0) return 0f
    val seconds = deltaNanos / NANOS_PER_SECOND
    return (a.distanceTo(b) / seconds).toFloat()
}

private const val NANOS_PER_SECOND = 1_000_000_000.0

/** The rotation block of a 4x4 is 3x3, and the 4x4 is stored with a column stride of four. */
private const val ROTATION_DIMENSION = 3
private const val MATRIX_COLUMN_STRIDE = 4
