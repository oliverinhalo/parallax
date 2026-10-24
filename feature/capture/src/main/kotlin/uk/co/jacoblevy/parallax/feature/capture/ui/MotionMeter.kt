package uk.co.jacoblevy.parallax.feature.capture.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.MotionBand

/**
 * How fast the phone is moving, as a band rather than a warning.
 *
 * The screen used to say one of two things -- "slow down", or nothing -- which leaves someone
 * holding a phone with no way to find the pace that works except by being told off for missing
 * it. Five segments, with the one they are in lit: standing still reads as a state rather than
 * as silence, and the two good bands sit visibly between the two bad ones, so the target is on
 * screen instead of being inferred from the absence of a complaint.
 *
 * Shown only while recording. Before the shutter there is no pace to hold.
 */
@Composable
internal fun MotionMeter(
    band: MotionBand,
    modifier: Modifier = Modifier,
) {
    val colors = ParallaxTheme.colors
    val lit = colorFor(band, accent = colors.accent, warning = colors.warning)
    val litColor by animateColorAsState(lit, label = "motionBand")

    Column(
        modifier.semantics { contentDescription = "Movement: ${band.label}. ${band.advice}" },
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(SEGMENT_GAP),
        ) {
            MotionBand.entries.forEach { segment ->
                val isCurrent = segment == band
                // The current segment is fully lit; its neighbours keep a trace so the scale
                // still reads as a scale when the phone is at one end of it.
                val target = if (isCurrent) 1f else TRACE_ALPHA
                val alpha by animateFloatAsState(target, label = "motionSegment")
                Box(
                    Modifier
                        .weight(if (isCurrent) CURRENT_WEIGHT else 1f)
                        .height(SEGMENT_HEIGHT)
                        .clip(RoundedCornerShape(SEGMENT_RADIUS))
                        .background(
                            if (isCurrent) {
                                litColor
                            } else {
                                colorFor(segment, colors.accent, colors.warning)
                                    .copy(alpha = alpha)
                            },
                        ),
                )
            }
        }
        Text(
            text = band.label,
            style = MaterialTheme.typography.labelSmall,
            color = litColor,
            modifier = Modifier.padding(top = Spacing.xs),
        )
    }
}

/**
 * One hue family, not a traffic light.
 *
 * The accent means "this is working" everywhere else in the app and amber means "attention",
 * which is the same thing being said here. Red and green would also be the one pairing the
 * design rules out, because the people it fails are the people who most need a glanceable
 * speed readout while walking backwards around a sofa.
 */
private fun colorFor(band: MotionBand, accent: Color, warning: Color): Color = when (band) {
    MotionBand.Still -> warning
    MotionBand.Slow -> accent.copy(alpha = SLOW_ALPHA)
    MotionBand.Ideal -> accent
    MotionBand.Fast -> accent.copy(alpha = SLOW_ALPHA)
    MotionBand.TooFast -> warning
}

private val SEGMENT_HEIGHT = 6.dp
private val SEGMENT_RADIUS = 3.dp
private val SEGMENT_GAP = 4.dp
private val METER_WIDTH = 180.dp

/** The lit segment is wider, so the band is readable without reading the label. */
private const val CURRENT_WEIGHT = 1.6f
private const val TRACE_ALPHA = 0.22f
private const val SLOW_ALPHA = 0.55f

/** Exposed so the capture screen can size the meter consistently with the shutter row. */
internal val MotionMeterWidth = METER_WIDTH
