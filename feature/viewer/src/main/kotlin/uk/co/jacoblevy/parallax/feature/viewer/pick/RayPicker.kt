package uk.co.jacoblevy.parallax.feature.viewer.pick

import uk.co.jacoblevy.parallax.core.model.Point3
import uk.co.jacoblevy.parallax.feature.viewer.camera.OrbitCamera
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt
import kotlin.math.tan

data class Ray(
    val originX: Float, val originY: Float, val originZ: Float,
    val directionX: Float, val directionY: Float, val directionZ: Float,
) {
    fun at(distance: Float): Point3 = Point3(
        originX + directionX * distance,
        originY + directionY * distance,
        originZ + directionZ * distance,
    )
}

data class Hit(val point: Point3, val normal: Point3, val distance: Float, val triangle: Int)

/**
 * The ray through a point on the screen.
 *
 * Built from the camera's own basis rather than by inverting a projection
 * matrix: the matrix is the renderer's business, and a picker that derives its
 * ray from the same state the camera exposes cannot disagree with what is on
 * screen because of a transposed matrix somewhere.
 */
fun screenRay(
    camera: OrbitCamera,
    viewportWidth: Int,
    viewportHeight: Int,
    screenX: Float,
    screenY: Float,
): Ray {
    val width = max(1, viewportWidth).toFloat()
    val height = max(1, viewportHeight).toFloat()
    // Normalised device coordinates, y up.
    val ndcX = 2f * (screenX / width) - 1f
    val ndcY = 1f - 2f * (screenY / height)
    val tanHalfFov = tan(0.5f * camera.fieldOfViewDegrees * OrbitCamera.DEGREES_TO_RADIANS)
    val aspect = width / height

    // Camera basis: forward points from the eye at the target.
    var forwardX = camera.targetX - camera.eyeX
    var forwardY = camera.targetY - camera.eyeY
    var forwardZ = camera.targetZ - camera.eyeZ
    val forwardLength = max(sqrt(forwardX * forwardX + forwardY * forwardY + forwardZ * forwardZ), 1e-9f)
    forwardX /= forwardLength
    forwardY /= forwardLength
    forwardZ /= forwardLength

    // right = normalize(forward x up), up = right x forward
    var rightX = forwardY * camera.upZ - forwardZ * camera.upY
    var rightY = forwardZ * camera.upX - forwardX * camera.upZ
    var rightZ = forwardX * camera.upY - forwardY * camera.upX
    val rightLength = max(sqrt(rightX * rightX + rightY * rightY + rightZ * rightZ), 1e-9f)
    rightX /= rightLength
    rightY /= rightLength
    rightZ /= rightLength
    val upX = rightY * forwardZ - rightZ * forwardY
    val upY = rightZ * forwardX - rightX * forwardZ
    val upZ = rightX * forwardY - rightY * forwardX

    val scaleX = ndcX * tanHalfFov * aspect
    val scaleY = ndcY * tanHalfFov
    var dirX = forwardX + rightX * scaleX + upX * scaleY
    var dirY = forwardY + rightY * scaleX + upY * scaleY
    var dirZ = forwardZ + rightZ * scaleX + upZ * scaleY
    val length = max(sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ), 1e-9f)
    dirX /= length
    dirY /= length
    dirZ /= length
    return Ray(camera.eyeX, camera.eyeY, camera.eyeZ, dirX, dirY, dirZ)
}

/**
 * Picks the nearest triangle a ray hits.
 *
 * A uniform grid over the triangles, not a BVH. The models here are a few
 * hundred thousand triangles and reasonably evenly spread, which is the case a
 * grid handles as well as a tree while being short enough to be obviously
 * correct; a BVH would be the right answer for a scene with wildly varying
 * triangle density, which a scan is not.
 */
class TrianglePicker(
    private val positions: FloatArray,
    private val indices: IntArray,
) {
    private val triangleCount = indices.size / 3
    private var minX = 0f
    private var minY = 0f
    private var minZ = 0f
    private var cellSize = 1f
    private var cellsX = 1
    private var cellsY = 1
    private var cellsZ = 1
    private val cellStart: IntArray
    private val cellTriangles: IntArray

    init {
        require(positions.size % 3 == 0) { "positions must be a multiple of three" }
        require(indices.size % 3 == 0) { "indices must be a multiple of three" }

        if (triangleCount == 0) {
            cellStart = intArrayOf(0, 0)
            cellTriangles = IntArray(0)
        } else {
            var maxX = Float.NEGATIVE_INFINITY
            var maxY = Float.NEGATIVE_INFINITY
            var maxZ = Float.NEGATIVE_INFINITY
            minX = Float.POSITIVE_INFINITY
            minY = Float.POSITIVE_INFINITY
            minZ = Float.POSITIVE_INFINITY
            var i = 0
            while (i < positions.size) {
                minX = min(minX, positions[i]); maxX = max(maxX, positions[i])
                minY = min(minY, positions[i + 1]); maxY = max(maxY, positions[i + 1])
                minZ = min(minZ, positions[i + 2]); maxZ = max(maxZ, positions[i + 2])
                i += 3
            }
            val extent = max(max(maxX - minX, maxY - minY), max(maxZ - minZ, 1e-4f))
            // Roughly one triangle per cell, which is where the grid's lookup
            // cost and its memory meet.
            val perSide = Math.cbrt(triangleCount.toDouble()).toInt().coerceIn(1, MAX_CELLS_PER_SIDE)
            cellSize = extent / perSide
            cellsX = (((maxX - minX) / cellSize).toInt() + 1).coerceAtLeast(1)
            cellsY = (((maxY - minY) / cellSize).toInt() + 1).coerceAtLeast(1)
            cellsZ = (((maxZ - minZ) / cellSize).toInt() + 1).coerceAtLeast(1)

            val cells = cellsX * cellsY * cellsZ
            val counts = IntArray(cells + 1)
            forEachTriangleCell { _, cell -> counts[cell + 1]++ }
            for (c in 0 until cells) counts[c + 1] += counts[c]
            cellStart = counts
            cellTriangles = IntArray(cellStart[cells])
            val cursor = cellStart.copyOf(cells)
            forEachTriangleCell { triangle, cell -> cellTriangles[cursor[cell]++] = triangle }
        }
    }

    private inline fun forEachTriangleCell(action: (triangle: Int, cell: Int) -> Unit) {
        for (triangle in 0 until triangleCount) {
            var lowX = Int.MAX_VALUE; var highX = Int.MIN_VALUE
            var lowY = Int.MAX_VALUE; var highY = Int.MIN_VALUE
            var lowZ = Int.MAX_VALUE; var highZ = Int.MIN_VALUE
            for (corner in 0 until 3) {
                val vertex = indices[triangle * 3 + corner] * 3
                val cx = cellIndex(positions[vertex] - minX, cellsX)
                val cy = cellIndex(positions[vertex + 1] - minY, cellsY)
                val cz = cellIndex(positions[vertex + 2] - minZ, cellsZ)
                lowX = min(lowX, cx); highX = max(highX, cx)
                lowY = min(lowY, cy); highY = max(highY, cy)
                lowZ = min(lowZ, cz); highZ = max(highZ, cz)
            }
            for (cz in lowZ..highZ) {
                for (cy in lowY..highY) {
                    for (cx in lowX..highX) {
                        action(triangle, (cz * cellsY + cy) * cellsX + cx)
                    }
                }
            }
        }
    }

    private fun cellIndex(offset: Float, count: Int): Int =
        (offset / cellSize).toInt().coerceIn(0, count - 1)

    /**
     * Nearest hit, or null.
     *
     * Every cell the ray's bounding box covers is visited rather than walking
     * the ray cell by cell. For a scan-sized model that is a few hundred cells
     * and one pass over them; a proper traversal would be faster and is the
     * sort of thing that goes subtly wrong on an axis-aligned ray.
     */
    fun intersect(ray: Ray, maxDistance: Float = Float.MAX_VALUE): Hit? {
        if (triangleCount == 0) return null
        var best: Hit? = null
        var bestDistance = maxDistance
        val visited = HashSet<Int>()

        var step = cellSize * HALF
        if (step <= 0f) step = 1f
        var travelled = 0f
        val limit = cellSize * (cellsX + cellsY + cellsZ).toFloat() * 2f
        while (travelled <= limit) {
            val cell = cellAt(ray.at(travelled))
            travelled += step
            if (cell >= 0 && visited.add(cell)) {
                for (slot in cellStart[cell] until cellStart[cell + 1]) {
                    val hit = intersectTriangle(ray, cellTriangles[slot], bestDistance) ?: continue
                    best = hit
                    bestDistance = hit.distance
                }
            }
        }
        return best
    }

    /** The grid cell a point falls in, or -1 when it is outside the grid. */
    private fun cellAt(point: Point3): Int {
        val cx = ((point.x - minX) / cellSize).toInt()
        val cy = ((point.y - minY) / cellSize).toInt()
        val cz = ((point.z - minZ) / cellSize).toInt()
        if (cx < 0 || cx >= cellsX) return -1
        if (cy < 0 || cy >= cellsY) return -1
        if (cz < 0 || cz >= cellsZ) return -1
        return (cz * cellsY + cy) * cellsX + cx
    }

    /** Möller-Trumbore, double-sided: a scan's winding is not to be trusted. */
    private fun intersectTriangle(ray: Ray, triangle: Int, maxDistance: Float): Hit? {
        val a = indices[triangle * 3] * 3
        val b = indices[triangle * 3 + 1] * 3
        val c = indices[triangle * 3 + 2] * 3

        val e1x = positions[b] - positions[a]
        val e1y = positions[b + 1] - positions[a + 1]
        val e1z = positions[b + 2] - positions[a + 2]
        val e2x = positions[c] - positions[a]
        val e2y = positions[c + 1] - positions[a + 1]
        val e2z = positions[c + 2] - positions[a + 2]

        val px = ray.directionY * e2z - ray.directionZ * e2y
        val py = ray.directionZ * e2x - ray.directionX * e2z
        val pz = ray.directionX * e2y - ray.directionY * e2x
        val determinant = e1x * px + e1y * py + e1z * pz
        if (abs(determinant) < 1e-12f) return null
        val inverse = 1f / determinant

        val tx = ray.originX - positions[a]
        val ty = ray.originY - positions[a + 1]
        val tz = ray.originZ - positions[a + 2]
        val u = (tx * px + ty * py + tz * pz) * inverse
        if (u < -1e-5f || u > 1f + 1e-5f) return null

        val qx = ty * e1z - tz * e1y
        val qy = tz * e1x - tx * e1z
        val qz = tx * e1y - ty * e1x
        val v = (ray.directionX * qx + ray.directionY * qy + ray.directionZ * qz) * inverse
        if (v < -1e-5f || u + v > 1f + 1e-5f) return null

        val distance = (e2x * qx + e2y * qy + e2z * qz) * inverse
        if (distance <= 1e-5f || distance >= maxDistance) return null

        var nx = e1y * e2z - e1z * e2y
        var ny = e1z * e2x - e1x * e2z
        var nz = e1x * e2y - e1y * e2x
        val length = max(sqrt(nx * nx + ny * ny + nz * nz), 1e-12f)
        nx /= length; ny /= length; nz /= length
        // Face the normal towards the camera, so a pin dropped on the inside
        // of a wall points into the room rather than through it.
        if (nx * ray.directionX + ny * ray.directionY + nz * ray.directionZ > 0f) {
            nx = -nx; ny = -ny; nz = -nz
        }
        return Hit(ray.at(distance), Point3(nx, ny, nz), distance, triangle)
    }

    private companion object {
        const val MAX_CELLS_PER_SIDE = 96
        const val HALF = 0.5f
    }
}
