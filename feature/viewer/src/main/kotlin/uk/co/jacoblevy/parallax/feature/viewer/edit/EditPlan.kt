package uk.co.jacoblevy.parallax.feature.viewer.edit

import uk.co.jacoblevy.parallax.core.model.BoundingBox
import uk.co.jacoblevy.parallax.core.model.ModelEdit
import uk.co.jacoblevy.parallax.core.model.Point2
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

/**
 * Edits resolved into something a renderer or an exporter can apply.
 *
 * Edits are stored as intent -- "crop to this box", "rescale so that measured
 * distance is really 2.4 m" -- rather than as modified geometry, so that the
 * original scan is never overwritten and the list can be re-ordered or undone.
 * This turns a list of intents into one transform and one keep test, which is
 * all either consumer needs.
 */
data class EditPlan(
    /** Row-major 4x4, applied to every vertex. */
    val transform: FloatArray = identityMatrix(),
    val cropBox: BoundingBox? = null,
    val lassoCuts: List<LassoCut> = emptyList(),
    val exposureStops: Float = 0f,
    val saturation: Float = 1f,
    val warmth: Float = 0f,
) {
    val isIdentity: Boolean
        get() = cropBox == null && lassoCuts.isEmpty() && exposureStops == 0f &&
            saturation == 1f && warmth == 0f && transform.contentEquals(identityMatrix())

    /** Apply the transform to a point. */
    fun transformed(x: Float, y: Float, z: Float): FloatArray = floatArrayOf(
        transform[0] * x + transform[1] * y + transform[2] * z + transform[3],
        transform[4] * x + transform[5] * y + transform[6] * z + transform[7],
        transform[8] * x + transform[9] * y + transform[10] * z + transform[11],
    )

    /**
     * Whether a vertex survives. Takes the vertex in its *original* position:
     * the crop box and the lasso were both drawn against what was on screen,
     * which is the transformed model, so the transform is applied first.
     */
    fun keeps(x: Float, y: Float, z: Float): Boolean {
        val point = transformed(x, y, z)
        cropBox?.let {
            if (point[0] < it.minX || point[0] > it.maxX) return false
            if (point[1] < it.minY || point[1] > it.maxY) return false
            if (point[2] < it.minZ || point[2] > it.maxZ) return false
        }
        for (cut in lassoCuts) {
            if (cut.contains(point[0], point[1], point[2])) return false
        }
        return true
    }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is EditPlan) return false
        return transform.contentEquals(other.transform) && cropBox == other.cropBox &&
            lassoCuts == other.lassoCuts && exposureStops == other.exposureStops &&
            saturation == other.saturation && warmth == other.warmth
    }

    override fun hashCode(): Int {
        var result = transform.contentHashCode()
        result = 31 * result + (cropBox?.hashCode() ?: 0)
        result = 31 * result + lassoCuts.hashCode()
        result = 31 * result + exposureStops.hashCode()
        result = 31 * result + saturation.hashCode()
        result = 31 * result + warmth.hashCode()
        return result
    }
}

/**
 * A region the user lassoed away, kept as the polygon plus the view it was
 * drawn in.
 *
 * Storing the view matrix with the polygon is what makes the cut reproducible:
 * a screen-space selection means nothing without knowing where the camera was,
 * and re-deriving it later from a saved camera would drift.
 */
data class LassoCut(val polygon: List<Point2>, val viewProjection: FloatArray) {
    fun contains(x: Float, y: Float, z: Float): Boolean {
        if (polygon.size < 3) return false
        val clipW = viewProjection[12] * x + viewProjection[13] * y +
            viewProjection[14] * z + viewProjection[15]
        // Behind the camera: it was not visible, so it was not selected.
        if (clipW <= 1e-6f) return false
        val clipX = viewProjection[0] * x + viewProjection[1] * y +
            viewProjection[2] * z + viewProjection[3]
        val clipY = viewProjection[4] * x + viewProjection[5] * y +
            viewProjection[6] * z + viewProjection[7]
        return pointInPolygon(clipX / clipW, clipY / clipW, polygon)
    }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is LassoCut) return false
        return polygon == other.polygon && viewProjection.contentEquals(other.viewProjection)
    }

    override fun hashCode(): Int = 31 * polygon.hashCode() + viewProjection.contentHashCode()
}

/**
 * Even-odd crossing test.
 *
 * A lasso drawn with a finger crosses itself constantly, and even-odd handles
 * that the way a user expects -- the overlap is outside the shape -- whereas a
 * winding test would keep it.
 */
internal fun pointInPolygon(x: Float, y: Float, polygon: List<Point2>): Boolean {
    var inside = false
    var j = polygon.size - 1
    for (i in polygon.indices) {
        val a = polygon[i]
        val b = polygon[j]
        if ((a.y > y) != (b.y > y)) {
            val crossing = a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x)
            if (x < crossing) inside = !inside
        }
        j = i
    }
    return inside
}

fun identityMatrix(): FloatArray = floatArrayOf(
    1f, 0f, 0f, 0f,
    0f, 1f, 0f, 0f,
    0f, 0f, 1f, 0f,
    0f, 0f, 0f, 1f,
)

/**
 * Fold a list of edits into one plan.
 *
 * Order matters and is the order the user made them in: a crop box drawn
 * before a re-level was drawn against the old orientation. Rather than try to
 * commute them, each geometric edit composes onto the running transform and
 * region edits are recorded against the transform as it stood when they were
 * made -- which is why the box is pre-transformed here rather than at use.
 */
fun planFor(edits: List<ModelEdit>): EditPlan {
    var plan = EditPlan()
    for (edit in edits) {
        plan = when (edit) {
            is ModelEdit.CropBox -> plan.copy(cropBox = intersect(plan.cropBox, edit.box))
            is ModelEdit.LassoDelete -> {
                val matrix = edit.viewMatrix.takeIf { it.size == MATRIX_ELEMENTS }?.toFloatArray()
                if (matrix == null || edit.screenPolygon.size < 3) {
                    plan
                } else {
                    plan.copy(lassoCuts = plan.lassoCuts + LassoCut(edit.screenPolygon, matrix))
                }
            }
            is ModelEdit.Relevel -> {
                val rotation = edit.rotation.takeIf { it.size == ROTATION_ELEMENTS }
                if (rotation == null) plan else {
                    plan.copy(transform = multiply(rotationMatrix(rotation), plan.transform))
                }
            }
            is ModelEdit.Rescale -> {
                val factor = edit.factor.toFloat()
                if (!factor.isFinite() || factor <= 0f) plan else {
                    plan.copy(transform = multiply(scaleMatrix(factor), plan.transform))
                }
            }
            is ModelEdit.TextureAdjust -> plan.copy(
                exposureStops = edit.exposureStops,
                saturation = edit.saturation,
                warmth = edit.warmth,
            )
        }
    }
    return plan
}

/** The rotation that levels a floor whose normal is [normal] onto +Y. */
fun levellingRotation(normalX: Float, normalY: Float, normalZ: Float): List<Float> {
    val length = kotlin.math.sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ)
    if (length < 1e-6f) return identityRotation()
    var nx = normalX / length
    var ny = normalY / length
    var nz = normalZ / length
    // Point the normal upwards first: a floor detected from below would
    // otherwise be levelled by turning the whole model upside down.
    if (ny < 0f) { nx = -nx; ny = -ny; nz = -nz }

    // Rotate n onto +Y about n x Y, by Rodrigues' formula. The order of the
    // cross product is the whole of it: Y x n turns the floor the wrong way.
    val axisX = -nz
    val axisY = 0f
    val axisZ = nx
    val axisLength = kotlin.math.sqrt(axisX * axisX + axisZ * axisZ)
    if (axisLength < 1e-6f) return identityRotation()
    val ux = axisX / axisLength
    val uy = axisY
    val uz = axisZ / axisLength
    val angle = kotlin.math.acos(ny.coerceIn(-1f, 1f))
    val c = cos(angle)
    val s = sin(angle)
    val t = 1f - c
    return listOf(
        t * ux * ux + c, t * ux * uy - s * uz, t * ux * uz + s * uy,
        t * ux * uy + s * uz, t * uy * uy + c, t * uy * uz - s * ux,
        t * ux * uz - s * uy, t * uy * uz + s * ux, t * uz * uz + c,
    )
}

fun identityRotation(): List<Float> = listOf(1f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 1f)

private const val MATRIX_ELEMENTS = 16
private const val ROTATION_ELEMENTS = 9

private fun intersect(existing: BoundingBox?, box: BoundingBox): BoundingBox {
    if (existing == null) return box
    return BoundingBox(
        minX = max(existing.minX, box.minX),
        minY = max(existing.minY, box.minY),
        minZ = max(existing.minZ, box.minZ),
        maxX = min(existing.maxX, box.maxX),
        maxY = min(existing.maxY, box.maxY),
        maxZ = min(existing.maxZ, box.maxZ),
    )
}

private fun rotationMatrix(rotation: List<Float>): FloatArray = floatArrayOf(
    rotation[0], rotation[1], rotation[2], 0f,
    rotation[3], rotation[4], rotation[5], 0f,
    rotation[6], rotation[7], rotation[8], 0f,
    0f, 0f, 0f, 1f,
)

private fun scaleMatrix(factor: Float): FloatArray = floatArrayOf(
    factor, 0f, 0f, 0f,
    0f, factor, 0f, 0f,
    0f, 0f, factor, 0f,
    0f, 0f, 0f, 1f,
)

/** Row-major 4x4 product, `a` applied after `b`. */
internal fun multiply(a: FloatArray, b: FloatArray): FloatArray {
    val out = FloatArray(MATRIX_ELEMENTS)
    for (row in 0 until 4) {
        for (column in 0 until 4) {
            var sum = 0f
            for (k in 0 until 4) sum += a[row * 4 + k] * b[k * 4 + column]
            out[row * 4 + column] = sum
        }
    }
    return out
}
