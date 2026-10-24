package uk.co.jacoblevy.parallax.feature.viewer.camera

import uk.co.jacoblevy.parallax.core.model.BoundingBox
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

/**
 * An orbiting camera, as pure state and pure transitions.
 *
 * Nothing here knows about Filament, gestures or Compose. That is deliberate:
 * camera feel is the part of a 3D viewer that is hardest to get right and
 * easiest to break, and it is only testable if it is separable. The renderer
 * reads [eyeX]/[eyeY]/[eyeZ] and [upX]/[upY]/[upZ] and does nothing else.
 *
 * Angles are radians. [yaw] turns about the world's +Y axis and [pitch] lifts
 * the camera above the horizon, so the model's up stays up however far the
 * camera has been dragged.
 */
data class OrbitCamera(
    val targetX: Float = 0f,
    val targetY: Float = 0f,
    val targetZ: Float = 0f,
    val distance: Float = 1f,
    val yaw: Float = 0f,
    val pitch: Float = 0.3f,
    val fieldOfViewDegrees: Float = 45f,
    /** Radians per second, decaying; what makes a flick keep going. */
    val yawVelocity: Float = 0f,
    val pitchVelocity: Float = 0f,
    /** The furthest the camera may be pulled back, from the model's size. */
    val minDistance: Float = 0.05f,
    val maxDistance: Float = 100f,
) {
    val eyeX: Float get() = targetX + distance * cos(pitch) * sin(yaw)
    val eyeY: Float get() = targetY + distance * sin(pitch)
    val eyeZ: Float get() = targetZ + distance * cos(pitch) * cos(yaw)

    val upX: Float get() = 0f
    val upY: Float get() = 1f
    val upZ: Float get() = 0f

    /** True once inertia has decayed below what a frame could show. */
    val isSettled: Boolean
        get() = abs(yawVelocity) < SETTLED_RADIANS_PER_SECOND &&
            abs(pitchVelocity) < SETTLED_RADIANS_PER_SECOND

    /**
     * Rotate by a drag.
     *
     * Pitch is clamped just short of the poles rather than at them: exactly
     * overhead the azimuth is undefined and the up vector degenerates, which
     * shows up as the model spinning on the spot.
     */
    fun rotatedBy(deltaYaw: Float, deltaPitch: Float): OrbitCamera = copy(
        yaw = wrapAngle(yaw + deltaYaw),
        pitch = (pitch + deltaPitch).coerceIn(-MAX_PITCH, MAX_PITCH),
    )

    /**
     * Slide the target across the view plane.
     *
     * The deltas are in screen fractions, converted to world units using the
     * distance and the field of view, so a drag moves whatever is under the
     * finger by about that much however far away the camera is.
     */
    fun pannedBy(deltaRight: Float, deltaUp: Float): OrbitCamera {
        val worldPerFraction = 2f * distance * tan(0.5f * fieldOfViewDegrees * DEGREES_TO_RADIANS)
        // The camera's right and up axes, from the same angles the eye uses.
        val rightX = cos(yaw)
        val rightZ = -sin(yaw)
        val upVectorX = -sin(pitch) * sin(yaw)
        val upVectorY = cos(pitch)
        val upVectorZ = -sin(pitch) * cos(yaw)
        val right = -deltaRight * worldPerFraction
        val up = deltaUp * worldPerFraction
        return copy(
            targetX = targetX + rightX * right + upVectorX * up,
            targetY = targetY + upVectorY * up,
            targetZ = targetZ + rightZ * right + upVectorZ * up,
        )
    }

    /** Pinch: a scale above one moves the camera closer. */
    fun zoomedBy(scale: Float): OrbitCamera {
        if (scale <= 0f) return this
        return copy(distance = (distance / scale).coerceIn(minDistance, maxDistance))
    }

    fun withFling(yawPerSecond: Float, pitchPerSecond: Float): OrbitCamera = copy(
        yawVelocity = yawPerSecond.coerceIn(-MAX_FLING, MAX_FLING),
        pitchVelocity = pitchPerSecond.coerceIn(-MAX_FLING, MAX_FLING),
    )

    fun stopped(): OrbitCamera = copy(yawVelocity = 0f, pitchVelocity = 0f)

    /**
     * Advance inertia by one frame.
     *
     * Exponential decay rather than a fixed subtraction, so the feel is the
     * same whether the frame took 8 ms or 40, and a flick never reverses
     * because a long frame overshot zero.
     */
    fun advanced(seconds: Float): OrbitCamera {
        if (seconds <= 0f || isSettled) return stopped()
        val decay = exp(-FLING_DECAY_PER_SECOND * seconds)
        val moved = rotatedBy(yawVelocity * seconds, pitchVelocity * seconds)
        return moved.copy(
            yawVelocity = yawVelocity * decay,
            pitchVelocity = pitchVelocity * decay,
        )
    }

    companion object {
        /** Just short of the pole, where the up vector would degenerate. */
        const val MAX_PITCH = 1.5533f
        const val DEGREES_TO_RADIANS = 0.017453292f
        private const val FLING_DECAY_PER_SECOND = 4.5f
        private const val MAX_FLING = 12f
        private const val SETTLED_RADIANS_PER_SECOND = 0.002f

        /** A little air around the model, so it does not touch the edges. */
        private const val FRAMING_MARGIN = 1.15f

        /**
         * A camera that sees all of [bounds].
         *
         * The distance comes from the bounding sphere rather than the box, so
         * the model stays in frame as it is orbited -- fitting the box as seen
         * from the starting angle means it leaves the screen the moment the
         * camera moves.
         */
        fun framing(
            bounds: BoundingBox,
            aspectRatio: Float = 1f,
            fieldOfViewDegrees: Float = 45f,
        ): OrbitCamera {
            val centreX = 0.5f * (bounds.minX + bounds.maxX)
            val centreY = 0.5f * (bounds.minY + bounds.maxY)
            val centreZ = 0.5f * (bounds.minZ + bounds.maxZ)
            // The true bounding sphere: half the box's diagonal.
            val width = bounds.width
            val height = bounds.height
            val depth = bounds.depth
            val radius = max(
                0.5f * sqrt(width * width + height * height + depth * depth),
                1e-3f,
            )
            // The vertical field of view is the binding one on a portrait
            // screen, which is where this app spends its life.
            val halfFov = 0.5f * fieldOfViewDegrees * DEGREES_TO_RADIANS
            val effective = if (aspectRatio < 1f) halfFov * aspectRatio else halfFov
            val distance = radius * FRAMING_MARGIN / max(sin(effective), 1e-3f)
            return OrbitCamera(
                targetX = centreX,
                targetY = centreY,
                targetZ = centreZ,
                distance = distance,
                yaw = DEFAULT_YAW,
                pitch = DEFAULT_PITCH,
                fieldOfViewDegrees = fieldOfViewDegrees,
                minDistance = max(radius * 0.05f, 1e-3f),
                maxDistance = max(distance * 8f, 1f),
            )
        }

        private const val DEFAULT_YAW = 0.6f
        private const val DEFAULT_PITCH = 0.35f
    }
}

/** Keep an angle in (-pi, pi] so it never grows without bound. */
internal fun wrapAngle(radians: Float): Float {
    val twoPi = 2f * Math.PI.toFloat()
    var value = radians % twoPi
    if (value > Math.PI.toFloat()) value -= twoPi
    if (value <= -Math.PI.toFloat()) value += twoPi
    return value
}
