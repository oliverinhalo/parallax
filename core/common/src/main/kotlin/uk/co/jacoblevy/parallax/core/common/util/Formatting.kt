package uk.co.jacoblevy.parallax.core.common.util

import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.math.roundToLong

private const val BYTES_PER_KIB = 1024.0
private const val MILLIS_PER_SECOND = 1000.0
private const val SECONDS_PER_MINUTE = 60L
private const val SECONDS_PER_HOUR = 3600L
private const val ALMOST_DONE_MILLIS = 5_000L

/** Below this many units, show one decimal place; above it, none. */
private const val ONE_DECIMAL_BELOW = 10.0

private const val THOUSAND = 1_000.0
private const val MILLION = 1_000_000.0

private const val INCHES_PER_METRE = 39.3700787401575
private const val INCHES_PER_FOOT = 12.0
private const val SQUARE_FEET_PER_SQUARE_METRE = 10.7639104
private const val CENTIMETRES_PER_METRE = 100.0
private const val MILLIMETRES_PER_METRE = 1000.0

/** Below a centimetre, millimetres read better; below a metre, centimetres. */
private const val MILLIMETRE_THRESHOLD_METRES = 0.01
private const val CENTIMETRE_THRESHOLD_METRES = 1.0

private const val PERCENT = 100

/** Human-readable byte count, using binary prefixes as Android's own UI does. */
fun formatBytes(bytes: Long): String {
    if (bytes < 0) return "—"
    if (bytes < BYTES_PER_KIB) return "$bytes B"
    val units = listOf("KB", "MB", "GB", "TB")
    var value = bytes.toDouble() / BYTES_PER_KIB
    var unitIndex = 0
    while (value >= BYTES_PER_KIB && unitIndex < units.lastIndex) {
        value /= BYTES_PER_KIB
        unitIndex++
    }
    // One decimal below 10 and none above reads better and keeps the column width stable.
    return if (value < ONE_DECIMAL_BELOW) {
        String.format(Locale.US, "%.1f %s", value, units[unitIndex])
    } else {
        String.format(Locale.US, "%.0f %s", value, units[unitIndex])
    }
}

/**
 * Duration as a short, glanceable string. Rounds rather than truncates, because "0 s remaining"
 * on a task that still has half a second to run looks broken.
 */
fun formatDuration(millis: Long): String {
    if (millis < 0) return "—"
    val totalSeconds = (millis / MILLIS_PER_SECOND).roundToLong()
    return when {
        totalSeconds < SECONDS_PER_MINUTE -> "${totalSeconds}s"
        totalSeconds < SECONDS_PER_HOUR -> {
            val minutes = totalSeconds / SECONDS_PER_MINUTE
            val seconds = totalSeconds % SECONDS_PER_MINUTE
            if (seconds == 0L) "${minutes}m" else "${minutes}m ${seconds}s"
        }
        else -> {
            val hours = totalSeconds / SECONDS_PER_HOUR
            val minutes = (totalSeconds % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE
            if (minutes == 0L) "${hours}h" else "${hours}h ${minutes}m"
        }
    }
}

/** An ETA phrased the way a progress screen should phrase it. */
fun formatEta(millis: Long?): String = when {
    millis == null -> "Estimating…"
    millis < ALMOST_DONE_MILLIS -> "Almost done"
    else -> "${formatDuration(millis)} left"
}

/** Percentage for display, clamped so a slightly-out-of-range progress value cannot show 101%. */
fun formatPercent(fraction: Float): String =
    "${(fraction.coerceIn(0f, 1f) * PERCENT).roundToInt()}%"

/** Triangle and point counts, abbreviated. */
fun formatCount(count: Long): String = when {
    count < THOUSAND -> count.toString()
    count < MILLION -> String.format(Locale.US, "%.1fK", count / THOUSAND)
    else -> String.format(Locale.US, "%.1fM", count / MILLION)
}

/** A length in metres rendered in the user's chosen units. */
fun formatLength(metres: Double, imperial: Boolean): String = if (imperial) {
    val totalInches = metres * INCHES_PER_METRE
    if (abs(totalInches) < INCHES_PER_FOOT) {
        String.format(Locale.US, "%.1f in", totalInches)
    } else {
        val feet = (totalInches / INCHES_PER_FOOT).toInt()
        val inches = totalInches - feet * INCHES_PER_FOOT
        String.format(Locale.US, "%d′ %.1f″", feet, inches)
    }
} else {
    when {
        abs(metres) < MILLIMETRE_THRESHOLD_METRES ->
            String.format(Locale.US, "%.1f mm", metres * MILLIMETRES_PER_METRE)
        abs(metres) < CENTIMETRE_THRESHOLD_METRES ->
            String.format(Locale.US, "%.1f cm", metres * CENTIMETRES_PER_METRE)
        else -> String.format(Locale.US, "%.3f m", metres)
    }
}

/** An area in square metres rendered in the user's chosen units. */
fun formatArea(squareMetres: Double, imperial: Boolean): String = if (imperial) {
    String.format(Locale.US, "%.1f ft²", squareMetres * SQUARE_FEET_PER_SQUARE_METRE)
} else {
    String.format(Locale.US, "%.2f m²", squareMetres)
}
