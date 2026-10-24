package uk.co.jacoblevy.parallax.feature.viewer.camera

import uk.co.jacoblevy.parallax.core.model.BoundingBox
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.sin

/**
 * First-person camera for walking a room scan.
 *
 * A room is the one thing an orbit camera cannot show you: you want to stand
 * in it. Movement is confined to the scan's own bounding box, slightly
 * inflated, because walking out through a wall into empty space is how a
 * walkthrough stops being useful -- there is nothing outside the scan to see,
 * and no way back except resetting the view.
 */
data class WalkCamera(
    val x: Float,
    val y: Float,
    val z: Float,
    val yaw: Float = 0f,
    val pitch: Float = 0f,
    val eyeHeightMetres: Float = DEFAULT_EYE_HEIGHT,
    val bounds: BoundingBox = BoundingBox(),
) {
    val eyeX: Float get() = x
    val eyeY: Float get() = y
    val eyeZ: Float get() = z

    /** Where the camera is looking, one metre ahead. */
    val forwardX: Float get() = cos(pitch) * sin(yaw)
    val forwardY: Float get() = sin(pitch)
    val forwardZ: Float get() = cos(pitch) * cos(yaw)

    /**
     * Turn. Pitch is clamped to just under a right angle either way: a walk
     * camera that can look straight up gives no sense of which way is forward.
     */
    fun looked(deltaYaw: Float, deltaPitch: Float): WalkCamera = copy(
        yaw = wrapAngle(yaw + deltaYaw),
        pitch = (pitch + deltaPitch).coerceIn(-MAX_PITCH, MAX_PITCH),
    )

    /**
     * Walk. [forward] and [strafe] are metres along the heading and across it;
     * the vertical component of the heading is dropped so that looking at the
     * floor does not drive the camera into it.
     */
    fun moved(forward: Float, strafe: Float): WalkCamera {
        val headingX = sin(yaw)
        val headingZ = cos(yaw)
        val nextX = x + headingX * forward + headingZ * strafe
        val nextZ = z + headingZ * forward - headingX * strafe
        return copy(
            x = nextX.coerceIn(bounds.minX - MARGIN_METRES, bounds.maxX + MARGIN_METRES),
            z = nextZ.coerceIn(bounds.minZ - MARGIN_METRES, bounds.maxZ + MARGIN_METRES),
        )
    }

    /** Raise or lower the eye, for a child's-eye or a standing view. */
    fun withEyeHeight(metres: Float): WalkCamera {
        val height = metres.coerceIn(MIN_EYE_HEIGHT, MAX_EYE_HEIGHT)
        return copy(eyeHeightMetres = height, y = bounds.minY + height)
    }

    companion object {
        const val DEFAULT_EYE_HEIGHT = 1.6f
        const val MIN_EYE_HEIGHT = 0.4f
        const val MAX_EYE_HEIGHT = 2.2f
        const val MAX_PITCH = 1.4f

        /** How far outside the scan the camera may stray, in metres. */
        const val MARGIN_METRES = 0.5f

        /**
         * Stand in the middle of the scan, facing its longest axis.
         *
         * Facing the long way down a room is almost always the view that shows
         * most of it; starting pointed at the nearest wall shows a wall.
         */
        fun standingIn(bounds: BoundingBox, eyeHeight: Float = DEFAULT_EYE_HEIGHT): WalkCamera {
            val facingZ = bounds.depth >= bounds.width
            return WalkCamera(
                x = 0.5f * (bounds.minX + bounds.maxX),
                y = bounds.minY + eyeHeight.coerceIn(MIN_EYE_HEIGHT, MAX_EYE_HEIGHT),
                z = 0.5f * (bounds.minZ + bounds.maxZ),
                yaw = if (facingZ) 0f else HALF_PI,
                pitch = 0f,
                eyeHeightMetres = eyeHeight.coerceIn(MIN_EYE_HEIGHT, MAX_EYE_HEIGHT),
                bounds = bounds,
            )
        }

        private const val HALF_PI = 1.5707964f
    }
}

/** Metres per second at a given stick deflection, for the on-screen pad. */
internal fun walkSpeedMetresPerSecond(deflection: Float): Float {
    val magnitude = deflection.coerceIn(-1f, 1f)
    // Squared response: small movements are for lining up a view, large ones
    // for crossing the room, and a linear stick does neither well.
    return magnitude * kotlin.math.abs(magnitude) * MAX_WALK_SPEED
}

private const val MAX_WALK_SPEED = 1.8f

/** Length of a two-axis stick deflection, clamped to the unit circle. */
internal fun stickMagnitude(x: Float, y: Float): Float = hypot(x, y).coerceAtMost(1f)
