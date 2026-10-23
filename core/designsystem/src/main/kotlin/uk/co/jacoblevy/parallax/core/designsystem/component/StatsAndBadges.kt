package uk.co.jacoblevy.parallax.core.designsystem.component

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxNumeric
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Radii
import uk.co.jacoblevy.parallax.core.designsystem.theme.Sizing
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing

/** Visual weight of a badge, mapped to a semantic colour. */
enum class BadgeTone { Neutral, Accent, Warning, Danger, Success }

/**
 * A small status pill.
 *
 * The whole pill is one semantics node with a spoken label, because an icon plus an abbreviated
 * number ("4.2K") read out piecemeal by TalkBack is meaningless.
 */
@Composable
fun ParallaxBadge(
    text: String,
    modifier: Modifier = Modifier,
    tone: BadgeTone = BadgeTone.Neutral,
    icon: ImageVector? = null,
    spokenLabel: String? = null,
) {
    val colors = ParallaxTheme.colors
    val (background, content) = when (tone) {
        BadgeTone.Neutral -> colors.surfaceOverlay to colors.textSecondary
        BadgeTone.Accent -> colors.accent.copy(alpha = 0.16f) to colors.accent
        BadgeTone.Warning -> colors.warning.copy(alpha = 0.16f) to colors.warning
        BadgeTone.Danger -> colors.danger.copy(alpha = 0.16f) to colors.danger
        BadgeTone.Success -> colors.success.copy(alpha = 0.16f) to colors.success
    }
    Row(
        modifier
            .clip(RoundedCornerShape(Radii.pill))
            .background(background)
            .padding(horizontal = Spacing.sm, vertical = Spacing.xs)
            .defaultMinSize(minHeight = Sizing.statPillHeight)
            .clearAndSetSemantics { contentDescription = spokenLabel ?: text },
        horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, tint = content, modifier = Modifier.size(14.dp))
        }
        Text(
            text = text,
            style = MaterialTheme.typography.labelMedium,
            color = content,
        )
    }
}

/**
 * A labelled numeric readout, as used across the capture HUD and the viewer info panel.
 *
 * The value uses the monospaced face with tabular figures so a live counter does not shift the
 * layout sideways as its digits change.
 */
@Composable
fun StatReadout(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    tone: BadgeTone = BadgeTone.Neutral,
    spokenLabel: String? = null,
) {
    val valueColor = when (tone) {
        BadgeTone.Neutral -> ParallaxTheme.colors.textPrimary
        BadgeTone.Accent -> ParallaxTheme.colors.accent
        BadgeTone.Warning -> ParallaxTheme.colors.warning
        BadgeTone.Danger -> ParallaxTheme.colors.danger
        BadgeTone.Success -> ParallaxTheme.colors.success
    }
    Column(
        modifier.clearAndSetSemantics {
            contentDescription = spokenLabel ?: "$label: $value"
        },
        horizontalAlignment = Alignment.Start,
    ) {
        Text(
            text = value,
            style = ParallaxNumeric.medium,
            color = valueColor,
            textAlign = TextAlign.Start,
        )
        Text(
            text = label.uppercase(),
            style = MaterialTheme.typography.labelSmall,
            color = ParallaxTheme.colors.textTertiary,
        )
    }
}

/** A compact readout for the capture overlay, where vertical space is scarce. */
@Composable
fun CompactStat(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    tone: BadgeTone = BadgeTone.Neutral,
) {
    val valueColor = when (tone) {
        BadgeTone.Accent -> ParallaxTheme.colors.accent
        BadgeTone.Warning -> ParallaxTheme.colors.warning
        BadgeTone.Danger -> ParallaxTheme.colors.danger
        BadgeTone.Success -> ParallaxTheme.colors.success
        BadgeTone.Neutral -> Color.White
    }
    Row(
        modifier.clearAndSetSemantics { contentDescription = "$label: $value" },
        horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = Color.White.copy(alpha = 0.6f),
        )
        Text(text = value, style = ParallaxNumeric.caption, color = valueColor)
    }
}

/** A chip used for mode switching and filters. */
@Composable
fun ParallaxChip(
    text: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
) {
    val colors = ParallaxTheme.colors
    val background = when {
        !enabled -> colors.surfaceOverlay.copy(alpha = 0.4f)
        selected -> colors.accent
        else -> colors.surfaceOverlay
    }
    val content = when {
        !enabled -> colors.textDisabled
        selected -> colors.onAccent
        else -> colors.textSecondary
    }
    Row(
        modifier
            .clip(RoundedCornerShape(Radii.pill))
            .background(background)
            .border(
                width = 1.dp,
                color = if (selected) Color.Transparent else colors.outlineSubtle,
                shape = RoundedCornerShape(Radii.pill),
            )
            .then(if (enabled) Modifier.clickable(onClick = onClick) else Modifier)
            .defaultMinSize(minHeight = Sizing.minTouchTarget)
            .padding(horizontal = Spacing.lg, vertical = Spacing.sm),
        horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, tint = content, modifier = Modifier.size(16.dp))
        }
        Text(text = text, style = MaterialTheme.typography.labelLarge, color = content)
    }
}
