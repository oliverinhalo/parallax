package uk.co.jacoblevy.parallax.core.designsystem.component

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Radii
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing

/**
 * A raised card.
 *
 * Elevation is a lighter surface plus a hairline border, not a drop shadow. Shadows on a
 * near-black background are invisible, so Material's elevation would leave every panel looking
 * flat and identical; a one-step-lighter fill is what actually reads as "in front".
 */
@Composable
fun ParallaxCard(
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(Spacing.lg),
    cornerRadius: Dp = Radii.lg,
    bordered: Boolean = true,
    onClick: (() -> Unit)? = null,
    content: @Composable () -> Unit,
) {
    val shape = RoundedCornerShape(cornerRadius)
    var surface = modifier
        .clip(shape)
        .background(ParallaxTheme.colors.surfaceRaised)
    if (bordered) {
        surface = surface.border(BorderStroke(1.dp, ParallaxTheme.colors.outlineSubtle), shape)
    }
    if (onClick != null) {
        surface = surface.clickable(onClick = onClick)
    }
    Box(surface.padding(contentPadding)) { content() }
}

/**
 * A translucent "glass" panel for overlays on the capture viewfinder.
 *
 * Deliberately a solid translucent fill rather than a real backdrop blur. Blurring the live
 * camera preview costs a full-screen render pass every frame, which on a mid-range device is the
 * difference between holding 60fps and not. The fill is dark and opaque enough that text stays
 * legible over any scene, which is the actual requirement.
 */
@Composable
fun GlassPanel(
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(Spacing.md),
    cornerRadius: Dp = Radii.lg,
    content: @Composable () -> Unit,
) {
    val shape = RoundedCornerShape(cornerRadius)
    Box(
        modifier
            .clip(shape)
            .background(ParallaxTheme.colors.surfaceGlass)
            // A faint light edge is what separates the panel from a bright scene behind it.
            .border(BorderStroke(1.dp, Color.White.copy(alpha = 0.08f)), shape)
            .padding(contentPadding),
    ) { content() }
}

/** A titled section with consistent heading treatment. */
@Composable
fun ParallaxSection(
    title: String,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
    content: @Composable () -> Unit,
) {
    Column(modifier) {
        Text(
            text = title,
            style = MaterialTheme.typography.titleMedium,
            color = ParallaxTheme.colors.textPrimary,
        )
        if (subtitle != null) {
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = ParallaxTheme.colors.textTertiary,
                modifier = Modifier.padding(top = Spacing.hairline),
            )
        }
        Box(Modifier.padding(top = Spacing.md)) { content() }
    }
}

/** A hairline divider at the subtle outline colour. */
@Composable
fun ParallaxDivider(modifier: Modifier = Modifier) {
    Box(
        modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(ParallaxTheme.colors.outlineSubtle),
    )
}
