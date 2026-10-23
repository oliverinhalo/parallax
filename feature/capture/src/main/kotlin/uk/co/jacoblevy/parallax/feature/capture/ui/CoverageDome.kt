package uk.co.jacoblevy.parallax.feature.capture.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Sizing
import uk.co.jacoblevy.parallax.core.model.CoverageMap
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * The coverage dome: which directions the subject has been seen from.
 *
 * Drawn as a polar chart rather than a 3D hemisphere. A perspective dome looks impressive and
 * is much harder to read at a glance with one hand on the phone: the back of a 3D dome is
 * behind its front, which is exactly the part people need to check. Flattened, every direction
 * is visible at once -- azimuth around, elevation outwards from the centre, so the middle is
 * "from above" and the rim is "from below".
 *
 * Colour comes from the design system's coverage ramp, which separates by lightness as well as
 * hue so it stays readable for the most common forms of colour blindness.
 */
@Composable
internal fun CoverageDome(
    coverage: CoverageMap,
    modifier: Modifier = Modifier,
    size: Dp = Sizing.coverageDomeSize,
) {
    val colors = ParallaxTheme.colors
    // Read the palette outside the draw lambda: a Canvas scope is not a composable scope.
    val ramp = remember(colors) {
        (0..RAMP_STEPS).map { step -> colors.coverageColor(step / RAMP_STEPS.toFloat()) }
    }
    val emptyColor = colors.surfaceOverlay.copy(alpha = EMPTY_ALPHA)
    val outlineColor = Color.White.copy(alpha = OUTLINE_ALPHA)

    Box(
        modifier
            .size(size)
            .semantics { contentDescription = spokenCoverage(coverage) },
    ) {
        Canvas(Modifier.size(size)) {
            val centre = Offset(this.size.width / 2f, this.size.height / 2f)
            val radius = min(this.size.width, this.size.height) / 2f - STROKE_PX

            val azimuthBins = coverage.azimuthBins
            val elevationBins = coverage.elevationBins
            if (azimuthBins <= 0 || elevationBins <= 0) return@Canvas

            val ringThickness = radius / elevationBins
            // Elevation runs from the rim inwards: bin 0 is "seen from below", which belongs on
            // the outside, because that is where the user's phone is when they crouch.
            for (elevation in 0 until elevationBins) {
                val inner = radius - (elevation + 1) * ringThickness
                val outer = radius - elevation * ringThickness
                for (azimuth in 0 until azimuthBins) {
                    val value = coverage.binAt(elevation, azimuth)
                    val colour = if (value <= 0f) {
                        emptyColor
                    } else {
                        ramp[(value * RAMP_STEPS).toInt().coerceIn(0, RAMP_STEPS)]
                    }
                    drawSector(
                        centre = centre,
                        innerRadius = inner.coerceAtLeast(0f),
                        outerRadius = outer,
                        startDegrees = azimuth * DEGREES_PER_TURN / azimuthBins,
                        sweepDegrees = DEGREES_PER_TURN / azimuthBins,
                        colour = colour,
                    )
                }
            }

            // A hairline circle so the dome reads as a shape over a bright scene.
            drawCircle(
                color = outlineColor,
                radius = radius,
                center = centre,
                style = Stroke(width = STROKE_PX),
            )
        }
    }
}

/**
 * Draw one annular sector.
 *
 * Built from an explicit path rather than `drawArc`, because `drawArc` with a stroke centres the
 * stroke on the arc and leaves seams between neighbouring bins; a filled path tiles exactly.
 */
private fun DrawScope.drawSector(
    centre: Offset,
    innerRadius: Float,
    outerRadius: Float,
    startDegrees: Float,
    sweepDegrees: Float,
    colour: Color,
) {
    val steps = SECTOR_STEPS
    val path = Path()
    for (step in 0..steps) {
        val angle = Math.toRadians(
            (startDegrees + sweepDegrees * step / steps).toDouble(),
        )
        val x = centre.x + outerRadius * cos(angle).toFloat()
        val y = centre.y + outerRadius * sin(angle).toFloat()
        if (step == 0) path.moveTo(x, y) else path.lineTo(x, y)
    }
    for (step in steps downTo 0) {
        val angle = Math.toRadians(
            (startDegrees + sweepDegrees * step / steps).toDouble(),
        )
        val x = centre.x + innerRadius * cos(angle).toFloat()
        val y = centre.y + innerRadius * sin(angle).toFloat()
        path.lineTo(x, y)
    }
    path.close()
    drawPath(path, colour)
}

/**
 * What TalkBack reads for the dome.
 *
 * A heatmap is useless to a screen reader, so it is described rather than drawn: how much is
 * covered, and which direction is still missing.
 */
private fun spokenCoverage(coverage: CoverageMap): String {
    val percent = (coverage.coverage * PERCENT).toInt()
    return "Coverage $percent percent"
}

private const val DEGREES_PER_TURN = 360f
private const val SECTOR_STEPS = 6
private const val STROKE_PX = 2f
private const val RAMP_STEPS = 16
private const val EMPTY_ALPHA = 0.35f
private const val OUTLINE_ALPHA = 0.25f
private const val PERCENT = 100
