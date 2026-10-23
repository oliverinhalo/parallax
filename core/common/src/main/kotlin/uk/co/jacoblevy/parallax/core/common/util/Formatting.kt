package uk.co.jacoblevy.parallax.core.common.util

import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/** Human-readable byte count, using binary prefixes as Android's own UI does. */
fun formatBytes(bytes: Long): String {
    if (bytes < 0) return "—"
    if (bytes < 1024) return "$bytes B"
    val units = listOf("KB", "MB", "GB", "TB")
    var value = bytes.toDouble() / 1024.0
    var unitIndex = 0
    while (value >= 1024.0 && unitIndex < units.lastIndex) {
        value /= 1024.0
        unitIndex++
    }
    // One decimal below 10 and none above reads better and keeps the column width stable.
    return if (value < 10.0) {
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
    val totalSeconds = (millis / 1000.0).roundToLong()
    return when {
        totalSeconds < 60 -> "${totalSeconds}s"
        totalSeconds < 3600 -> {
            val minutes = totalSeconds / 60
            val seconds = totalSeconds % 60
            if (seconds == 0L) "${minutes}m" else "${minutes}m ${seconds}s"
        }
        else -> {
            val hours = totalSeconds / 3600
            val minutes = (totalSeconds % 3600) / 60
            if (minutes == 0L) "${hours}h" else "${hours}h ${minutes}m"
        }
    }
}

/** An ETA phrased the way a progress screen should phrase it. */
fun formatEta(millis: Long?): String = when {
    millis == null -> "Estimating…"
    millis < 5_000 -> "Almost done"
    else -> "${formatDuration(millis)} left"
}

/** Percentage for display, clamped so a slightly-out-of-range progress value cannot show 101%. */
fun formatPercent(fraction: Float): String =
    "${(fraction.coerceIn(0f, 1f) * 100).roundToInt()}%"

/** Triangle and point counts, abbreviated. */
fun formatCount(count: Long): String = when {
    count < 1_000 -> count.toString()
    count < 1_000_000 -> String.format(Locale.US, "%.1fK", count / 1_000.0)
    else -> String.format(Locale.US, "%.1fM", count / 1_000_000.0)
}

/** A length in metres rendered in the user's chosen units. */
fun formatLength(metres: Double, imperial: Boolean): String = if (imperial) {
    val totalInches = metres * 39.3700787401575
    if (abs(totalInches) < 12.0) {
        String.format(Locale.US, "%.1f in", totalInches)
    } else {
        val feet = (totalInches / 12.0).toInt()
        val inches = totalInches - feet * 12.0
        String.format(Locale.US, "%d′ %.1f″", feet, inches)
    }
} else {
    when {
        abs(metres) < 0.01 -> String.format(Locale.US, "%.1f mm", metres * 1000)
        abs(metres) < 1.0 -> String.format(Locale.US, "%.1f cm", metres * 100)
        else -> String.format(Locale.US, "%.3f m", metres)
    }
}

/** An area in square metres rendered in the user's chosen units. */
fun formatArea(squareMetres: Double, imperial: Boolean): String = if (imperial) {
    String.format(Locale.US, "%.1f ft²", squareMetres * 10.7639104)
} else {
    String.format(Locale.US, "%.2f m²", squareMetres)
}
