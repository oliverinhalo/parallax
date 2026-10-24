package uk.co.jacoblevy.parallax.core.designsystem.component

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import uk.co.jacoblevy.parallax.core.common.util.formatPercent
import uk.co.jacoblevy.parallax.core.designsystem.motion.Motion
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxNumeric
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Sizing
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing

/**
 * A determinate progress bar.
 *
 * The value is animated with a monotonic easing and never interpolated downwards: a progress bar
 * that visibly retreats destroys confidence in the estimate behind it, so a lower value snaps
 * instead of animating back.
 */
@Composable
fun ParallaxProgressBar(
    progress: Float,
    modifier: Modifier = Modifier,
    height: Dp = Sizing.progressBarHeight,
    indeterminate: Boolean = false,
) {
    val target = progress.coerceIn(0f, 1f)
    val animated by animateFloatAsState(
        targetValue = target,
        animationSpec = Motion.progress(),
        label = "progress",
    )
    val shape = RoundedCornerShape(percent = 50)
    // Read the theme here: a Canvas draw lambda is not a composable scope, so
    // ParallaxTheme.colors cannot be touched inside it.
    val trackColor = ParallaxTheme.colors.progressTrack
    val fillStart = ParallaxTheme.colors.accentMuted
    val fillEnd = ParallaxTheme.colors.progressFill

    Box(
        modifier
            .fillMaxWidth()
            .height(height)
            .clip(shape)
            .background(trackColor)
            .semantics {
                progressBarRangeInfo = if (indeterminate) {
                    ProgressBarRangeInfo.Indeterminate
                } else {
                    ProgressBarRangeInfo(current = target, range = 0f..1f)
                }
            },
    ) {
        if (!indeterminate) {
            Canvas(Modifier.fillMaxWidth().height(height)) {
                val width = size.width * animated
                if (width <= 0f) return@Canvas
                drawRoundRect(
                    brush = Brush.horizontalGradient(listOf(fillStart, fillEnd)),
                    topLeft = Offset.Zero,
                    size = Size(width, size.height),
                    cornerRadius = CornerRadius(size.height / 2f),
                )
            }
        }
    }
}

/** Progress with a stage label and a monospaced percentage, as the processing screen shows it. */
@Composable
fun LabelledProgress(
    label: String,
    progress: Float,
    modifier: Modifier = Modifier,
    detail: String? = null,
) {
    Column(modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.labelLarge,
                color = ParallaxTheme.colors.textPrimary,
            )
            Text(
                text = formatPercent(progress),
                style = ParallaxNumeric.small,
                color = ParallaxTheme.colors.accent,
            )
        }
        ParallaxProgressBar(
            progress = progress,
            modifier = Modifier.padding(top = Spacing.sm),
        )
        if (detail != null) {
            Text(
                text = detail,
                style = MaterialTheme.typography.bodySmall,
                color = ParallaxTheme.colors.textTertiary,
                modifier = Modifier.padding(top = Spacing.xs),
            )
        }
    }
}
