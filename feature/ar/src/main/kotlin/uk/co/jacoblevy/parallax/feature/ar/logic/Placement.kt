package uk.co.jacoblevy.parallax.feature.ar.logic

import uk.co.jacoblevy.parallax.core.model.BoundingBox
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.round
import kotlin.math.sin

/** How far along the user is in putting a scan on the floor. */
enum class PlacementPhase {
    /** Nothing found yet: move the phone around. */
    Looking,

    /** A surface is tracked and waiting for a tap. */
    Ready,

    /** Placed, and now adjustable. */
    Placed,
}

/**
 * Where a scan sits in the room, as pure state.
 *
 * None of this knows about ARCore. The session hands in a point on a tracked
 * surface and this decides what the model's transform is, which keeps the
 * part that is easy to get wrong -- putting the model's floor on the room's
 * floor rather than its centre, snapping rotation, keeping scale sane --
 * testable without a camera.
 */
data class Placement(
    val phase: PlacementPhase = PlacementPhase.Looking,
    /** The tapped point on the surface, in AR world space. */
    val anchorX: Float = 0f,
    val anchorY: Float = 0f,
    val anchorZ: Float = 0f,
    val yaw: Float = 0f,
    val scale: Float = 1f,
    /** The scan's own bounds, used to sit it on the surface and to size it. */
    val bounds: BoundingBox = BoundingBox(),
    val locked: Boolean = false,
) {
    val isPlaced: Boolean get() = phase == PlacementPhase.Placed

    fun surfaceFound(): Placement =
        if (phase == PlacementPhase.Looking) copy(phase = PlacementPhase.Ready) else this

    fun surfaceLost(): Placement =
        if (phase == PlacementPhase.Ready) copy(phase = PlacementPhase.Looking) else this

    /**
     * Put the model down at a point on the surface.
     *
     * Life-size by default. A room scan placed at life size will not fit in
     * the room it is being shown in, so anything taller than head height is
     * scaled down to a model on a table -- which is what someone showing a
     * room scan to another person in that room actually wants.
     */
    fun placeAt(x: Float, y: Float, z: Float, modelBounds: BoundingBox): Placement {
        val tall = modelBounds.height > LIFE_SIZE_LIMIT_METRES ||
            modelBounds.width > LIFE_SIZE_LIMIT_METRES ||
            modelBounds.depth > LIFE_SIZE_LIMIT_METRES
        val initialScale = if (tall) {
            val longest = maxOf(modelBounds.width, modelBounds.height, modelBounds.depth)
            (TABLE_TOP_SIZE_METRES / longest).coerceIn(MIN_SCALE, 1f)
        } else {
            1f
        }
        return copy(
            phase = PlacementPhase.Placed,
            anchorX = x,
            anchorY = y,
            anchorZ = z,
            bounds = modelBounds,
            scale = initialScale,
            yaw = 0f,
            locked = false,
        )
    }

    fun rotatedBy(radians: Float): Placement =
        if (locked) this else copy(yaw = wrap(yaw + radians))

    fun scaledBy(factor: Float): Placement {
        if (locked || factor <= 0f) return this
        return copy(scale = (scale * factor).coerceIn(MIN_SCALE, MAX_SCALE))
    }

    /** Drag the model across the surface. Height never changes. */
    fun movedBy(deltaX: Float, deltaZ: Float): Placement =
        if (locked) this else copy(anchorX = anchorX + deltaX, anchorZ = anchorZ + deltaZ)

    /**
     * Snap to the nearest right angle.
     *
     * Rooms are rectangular and so are the rooms they are shown in, so lining
     * a scan up by eye is fiddly and almost right is worse than exact.
     */
    fun snappedToRightAngle(): Placement {
        if (locked) return this
        val quarter = HALF_PI
        return copy(yaw = wrap(round(yaw / quarter) * quarter))
    }

    fun withLock(locked: Boolean): Placement = copy(locked = locked)

    fun reset(): Placement = Placement(phase = PlacementPhase.Ready, bounds = bounds)

    /**
     * Row-major 4x4 model transform.
     *
     * Translation includes `-bounds.minY * scale`, which is the whole point:
     * the model's own origin is wherever the scan's world happened to be, so
     * without it a scan sinks into the floor by however far its lowest point
     * was below zero.
     */
    fun modelMatrix(): FloatArray {
        val c = cos(yaw)
        val s = sin(yaw)
        val lift = -bounds.minY * scale
        return floatArrayOf(
            scale * c, 0f, scale * s, anchorX,
            0f, scale, 0f, anchorY + lift,
            -scale * s, 0f, scale * c, anchorZ,
            0f, 0f, 0f, 1f,
        )
    }

    /** The model's footprint in metres, as placed. */
    val footprintMetres: Pair<Float, Float>
        get() {
            val c = abs(cos(yaw))
            val s = abs(sin(yaw))
            val width = bounds.width * scale
            val depth = bounds.depth * scale
            return (width * c + depth * s) to (width * s + depth * c)
        }

    /** Height above the surface, in metres, as placed. */
    val heightMetres: Float get() = bounds.height * scale

    companion object {
        const val MIN_SCALE = 0.02f
        const val MAX_SCALE = 4f

        /**
         * Above this, in metres, a scan is assumed not to fit in the room it
         * is being shown in and is placed as a tabletop model instead.
         */
        const val LIFE_SIZE_LIMIT_METRES = 2.2f

        /** Longest side of a scaled-down scan, in metres. */
        const val TABLE_TOP_SIZE_METRES = 0.4f

        private const val HALF_PI = 1.5707964f
    }
}

internal fun wrap(radians: Float): Float {
    val twoPi = 2f * Math.PI.toFloat()
    var value = radians % twoPi
    if (value > Math.PI.toFloat()) value -= twoPi
    if (value <= -Math.PI.toFloat()) value += twoPi
    return value
}
