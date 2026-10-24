package uk.co.jacoblevy.parallax.feature.viewer.measure

import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.Point3
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.math.sqrt

/** Which kind of measurement the user is placing. */
enum class MeasureTool(val label: String, val pointsNeeded: Int) {
    Distance("Distance", 2),
    Height("Height", 2),
    Area("Area", 3),
}

/**
 * Points placed so far, and what they will become.
 *
 * Area is the one that cannot say in advance how many points it needs, so it
 * accumulates until the user says done; the other two complete themselves on
 * the last point, because asking for confirmation of a two-tap measurement is
 * one tap too many.
 */
data class MeasureDraft(
    val tool: MeasureTool,
    val points: List<Point3> = emptyList(),
) {
    val isComplete: Boolean
        get() = when (tool) {
            MeasureTool.Area -> false  // only ever finished explicitly
            else -> points.size >= tool.pointsNeeded
        }

    val canFinish: Boolean get() = points.size >= tool.pointsNeeded

    fun withPoint(point: Point3): MeasureDraft = when (tool) {
        MeasureTool.Area -> copy(points = points + point)
        // Placing a third point on a two-point tool starts again from it,
        // which is what a user who mis-tapped will try before looking for undo.
        else -> if (points.size >= tool.pointsNeeded) copy(points = listOf(point))
        else copy(points = points + point)
    }

    fun withoutLastPoint(): MeasureDraft = copy(points = points.dropLast(1))

    /** The live value as the points stand, for the label under the finger. */
    fun previewMetres(): Double? = when {
        points.size < 2 -> null
        tool == MeasureTool.Distance -> distanceMetres(points[0], points[1])
        tool == MeasureTool.Height -> abs(points[1].y - points[0].y).toDouble()
        points.size < 3 -> null
        else -> polygonAreaSquareMetres(points)
    }

    fun build(id: String, label: String): Measurement? = when (tool) {
        MeasureTool.Distance -> points.takeIf { it.size >= 2 }?.let {
            Measurement.Distance(
                id = id, label = label,
                fromX = it[0].x, fromY = it[0].y, fromZ = it[0].z,
                toX = it[1].x, toY = it[1].y, toZ = it[1].z,
            )
        }
        MeasureTool.Height -> points.takeIf { it.size >= 2 }?.let {
            Measurement.Height(id = id, label = label, baseY = it[0].y, topY = it[1].y)
        }
        MeasureTool.Area -> points.takeIf { it.size >= 3 }?.let {
            Measurement.Area(id = id, label = label, points = it)
        }
    }
}

fun distanceMetres(from: Point3, to: Point3): Double {
    val dx = (to.x - from.x).toDouble()
    val dy = (to.y - from.y).toDouble()
    val dz = (to.z - from.z).toDouble()
    return sqrt(dx * dx + dy * dy + dz * dz)
}

/**
 * Area of a polygon in 3D, by Newell's method.
 *
 * The shoelace formula needs a plane to work in, and the points a user taps on
 * a scanned floor are never quite coplanar. Newell's sum gives the area of the
 * best-fit plane's projection directly, which is both the right answer for a
 * planar polygon and a sensible one for a slightly bumpy floor.
 */
fun polygonAreaSquareMetres(points: List<Point3>): Double {
    if (points.size < 3) return 0.0
    var nx = 0.0
    var ny = 0.0
    var nz = 0.0
    for (i in points.indices) {
        val current = points[i]
        val next = points[(i + 1) % points.size]
        nx += (current.y - next.y).toDouble() * (current.z + next.z).toDouble()
        ny += (current.z - next.z).toDouble() * (current.x + next.x).toDouble()
        nz += (current.x - next.x).toDouble() * (current.y + next.y).toDouble()
    }
    return 0.5 * sqrt(nx * nx + ny * ny + nz * nz)
}

/** The area of a saved measurement, which the model type does not compute. */
val Measurement.Area.squareMetres: Double get() = polygonAreaSquareMetres(points)

// -------------------------------------------------------------- formatting ---

private const val CENTIMETRES_PER_METRE = 100
private const val MILLIMETRES_PER_METRE = 1000
private const val INCHES_PER_METRE = 39.3700787
private const val INCHES_PER_FOOT = 12
private const val SQUARE_FEET_PER_SQUARE_METRE = 10.7639104
private const val CENTIMETRE_CUTOFF_METRES = 1.0
private const val MILLIMETRE_CUTOFF_METRES = 0.01

/**
 * A length, in the units the reader asked for.
 *
 * Switching unit at a metre rather than always using metres: "0.043 m" is a
 * number you have to decode, "4.3 cm" is one you can picture. Below a
 * centimetre it goes to millimetres for the same reason.
 */
fun formatLength(metres: Double, imperial: Boolean): String {
    if (imperial) {
        val totalInches = metres * INCHES_PER_METRE
        val feet = (totalInches / INCHES_PER_FOOT).toInt()
        val inches = totalInches - feet * INCHES_PER_FOOT
        return if (feet > 0) {
            "$feet′ ${formatDecimal(inches, 1)}″"
        } else {
            "${formatDecimal(totalInches, 2)}″"
        }
    }
    val magnitude = abs(metres)
    return when {
        magnitude < MILLIMETRE_CUTOFF_METRES ->
            "${(metres * MILLIMETRES_PER_METRE).roundToInt()} mm"
        magnitude < CENTIMETRE_CUTOFF_METRES ->
            "${formatDecimal(metres * CENTIMETRES_PER_METRE, 1)} cm"
        else -> "${formatDecimal(metres, 2)} m"
    }
}

fun formatArea(squareMetres: Double, imperial: Boolean): String = if (imperial) {
    "${formatDecimal(squareMetres * SQUARE_FEET_PER_SQUARE_METRE, 1)} sq ft"
} else {
    "${formatDecimal(squareMetres, 2)} m²"
}

/**
 * Fixed-point formatting without `String.format`.
 *
 * `String.format` is locale-sensitive, and a measurement that reads "1,25 m"
 * in one locale and "1.25 m" in another is a support ticket waiting to happen
 * when the two appear in the same exported file.
 */
internal fun formatDecimal(value: Double, places: Int): String {
    var scale = 1L
    repeat(places) { scale *= 10 }
    val scaled = (abs(value) * scale).roundToInt().toLong()
    val whole = scaled / scale
    val fraction = scaled % scale
    val sign = if (value < 0 && scaled != 0L) "-" else ""
    if (places == 0) return "$sign$whole"
    return "$sign$whole." + fraction.toString().padStart(places, '0')
}
