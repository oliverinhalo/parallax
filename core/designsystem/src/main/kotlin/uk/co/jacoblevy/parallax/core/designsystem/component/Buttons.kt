package uk.co.jacoblevy.parallax.core.designsystem.component

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import uk.co.jacoblevy.parallax.core.designsystem.motion.Motion
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Radii
import uk.co.jacoblevy.parallax.core.designsystem.theme.Sizing
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import androidx.compose.foundation.shape.RoundedCornerShape

/**
 * The primary action button.
 *
 * Presses scale the button very slightly rather than only changing its colour. On a dark UI a
 * ripple is nearly invisible, and a scale gives immediate physical feedback even when the finger
 * covers the button entirely.
 */
@Composable
fun ParallaxButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    loading: Boolean = false,
    icon: ImageVector? = null,
    contentDescription: String? = null,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val pressed by interactionSource.collectIsPressedAsState()
    val scale by animateFloatAsState(
        targetValue = if (pressed) 0.97f else 1f,
        animationSpec = Motion.responsiveSpring(),
        label = "buttonScale",
    )

    Button(
        onClick = onClick,
        // A button that is loading must not be re-tappable, but it should still look enabled:
        // greying it out as it starts work reads as a failure rather than as progress.
        enabled = enabled && !loading,
        modifier = modifier
            .scale(scale)
            .defaultMinSize(minHeight = Sizing.minTouchTarget)
            .semantics { contentDescription?.let { this.contentDescription = it } },
        shape = RoundedCornerShape(Radii.md),
        interactionSource = interactionSource,
        colors = ButtonDefaults.buttonColors(
            containerColor = ParallaxTheme.colors.accent,
            contentColor = ParallaxTheme.colors.onAccent,
            disabledContainerColor = ParallaxTheme.colors.surfaceOverlay,
            disabledContentColor = ParallaxTheme.colors.textDisabled,
        ),
        contentPadding = ButtonDefaults.ContentPadding,
    ) {
        if (loading) {
            CircularProgressIndicator(
                modifier = Modifier.size(Sizing.iconSmall),
                strokeWidth = 2.dp,
                color = ParallaxTheme.colors.onAccent,
            )
            Spacer(Modifier.width(Spacing.sm))
        } else if (icon != null) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(Sizing.iconSmall))
            Spacer(Modifier.width(Spacing.sm))
        }
        Text(text, style = androidx.compose.material3.MaterialTheme.typography.labelLarge)
    }
}

/** Secondary action: outlined, same geometry as the primary so they align in a row. */
@Composable
fun ParallaxOutlinedButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    icon: ImageVector? = null,
    destructive: Boolean = false,
) {
    val contentColor =
        if (destructive) ParallaxTheme.colors.danger else ParallaxTheme.colors.textPrimary
    OutlinedButton(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.defaultMinSize(minHeight = Sizing.minTouchTarget),
        shape = RoundedCornerShape(Radii.md),
        colors = ButtonDefaults.outlinedButtonColors(
            contentColor = contentColor,
            disabledContentColor = ParallaxTheme.colors.textDisabled,
        ),
        border = androidx.compose.foundation.BorderStroke(
            width = 1.dp,
            color = if (enabled) {
                if (destructive) ParallaxTheme.colors.danger else ParallaxTheme.colors.outline
            } else {
                ParallaxTheme.colors.outlineSubtle
            },
        ),
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(Sizing.iconSmall))
            Spacer(Modifier.width(Spacing.sm))
        }
        Text(text, style = androidx.compose.material3.MaterialTheme.typography.labelLarge)
    }
}

/** Low-emphasis action, for dismissals and tertiary links. */
@Composable
fun ParallaxTextButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    destructive: Boolean = false,
) {
    TextButton(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.defaultMinSize(minHeight = Sizing.minTouchTarget),
        colors = ButtonDefaults.textButtonColors(
            contentColor = if (destructive) {
                ParallaxTheme.colors.danger
            } else {
                ParallaxTheme.colors.accent
            },
            disabledContentColor = ParallaxTheme.colors.textDisabled,
        ),
    ) {
        Text(text, style = androidx.compose.material3.MaterialTheme.typography.labelLarge)
    }
}

/**
 * A square icon button sized to the minimum touch target even when the icon is small.
 *
 * [contentDescription] is required rather than nullable: an icon-only control with no label is
 * invisible to TalkBack, and making it optional is how that happens.
 */
@Composable
fun ParallaxIconButton(
    icon: ImageVector,
    contentDescription: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    selected: Boolean = false,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val pressed by interactionSource.collectIsPressedAsState()
    val scale by animateFloatAsState(
        targetValue = if (pressed) 0.92f else 1f,
        animationSpec = Motion.responsiveSpring(),
        label = "iconButtonScale",
    )
    androidx.compose.material3.IconButton(
        onClick = onClick,
        enabled = enabled,
        interactionSource = interactionSource,
        modifier = modifier
            .size(Sizing.minTouchTarget)
            .scale(scale),
        colors = androidx.compose.material3.IconButtonDefaults.iconButtonColors(
            contentColor = if (selected) {
                ParallaxTheme.colors.accent
            } else {
                ParallaxTheme.colors.textSecondary
            },
            disabledContentColor = ParallaxTheme.colors.textDisabled,
        ),
    ) {
        Icon(icon, contentDescription = contentDescription, modifier = Modifier.size(Sizing.iconMedium))
    }
}

/** A horizontal row of actions with consistent spacing. */
@Composable
fun ParallaxActionRow(
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    Row(
        modifier = modifier.padding(horizontal = Spacing.screenHorizontal),
        horizontalArrangement = Arrangement.spacedBy(Spacing.md),
        verticalAlignment = Alignment.CenterVertically,
    ) { content() }
}
