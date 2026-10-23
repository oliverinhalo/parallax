package uk.co.jacoblevy.parallax.feature.settings

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.dp
import uk.co.jacoblevy.parallax.core.common.util.formatBytes
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxChip
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Radii
import uk.co.jacoblevy.parallax.core.designsystem.theme.Sizing
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.StorageBreakdown

/**
 * A labelled switch row.
 *
 * The whole row is the touch target and carries the semantics, so TalkBack announces the
 * setting's name, its explanation and its state as one switch rather than as a label followed
 * by an unlabelled control.
 */
@Composable
internal fun SwitchRow(
    title: String,
    subtitle: String?,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    Row(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(Radii.sm))
            .clickable(enabled = enabled) { onCheckedChange(!checked) }
            .padding(vertical = Spacing.md)
            .semantics(mergeDescendants = true) {
                role = Role.Switch
                contentDescription = if (subtitle == null) title else "$title. $subtitle"
                stateDescription = if (checked) "On" else "Off"
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyLarge,
                color = if (enabled) {
                    ParallaxTheme.colors.textPrimary
                } else {
                    ParallaxTheme.colors.textDisabled
                },
            )
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = ParallaxTheme.colors.textTertiary,
                )
            }
        }
        Spacer(Modifier.size(Spacing.md))
        Switch(
            checked = checked,
            onCheckedChange = null,
            enabled = enabled,
            colors = SwitchDefaults.colors(
                checkedTrackColor = ParallaxTheme.colors.accent,
                checkedThumbColor = ParallaxTheme.colors.onAccent,
            ),
        )
    }
}

/** A row of mutually exclusive choices, as chips. */
@Composable
internal fun <T> ChoiceRow(
    title: String,
    options: List<T>,
    selected: T,
    label: (T) -> String,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
    enabledFor: (T) -> Boolean = { true },
) {
    Column(modifier.fillMaxWidth().padding(vertical = Spacing.md)) {
        Text(
            text = title,
            style = MaterialTheme.typography.bodyLarge,
            color = ParallaxTheme.colors.textPrimary,
        )
        if (subtitle != null) {
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = ParallaxTheme.colors.textTertiary,
            )
        }
        Spacer(Modifier.height(Spacing.sm))
        Row(horizontalArrangement = Arrangement.spacedBy(Spacing.sm)) {
            options.forEach { option ->
                ParallaxChip(
                    text = label(option),
                    selected = option == selected,
                    enabled = enabledFor(option),
                    onClick = { onSelect(option) },
                )
            }
        }
    }
}

/**
 * The storage dashboard's stacked bar.
 *
 * Categories are drawn in the order the breakdown lists them, with reclaimable ones first, so
 * the part of the bar the user can act on is adjacent to the button that acts on it. Segments
 * below a pixel are still drawn at one pixel: a category that exists should be visible.
 */
@Composable
internal fun StorageBar(breakdown: StorageBreakdown, modifier: Modifier = Modifier) {
    val categories = breakdown.categories
    val total = breakdown.totalBytes
    val colors = ParallaxTheme.colors
    // Read the palette outside the draw lambda: a Canvas scope is not a composable scope.
    val palette = categories.indices.map { index ->
        val fraction = if (categories.size == 1) {
            0.5f
        } else {
            index.toFloat() / (categories.size - 1)
        }
        colors.coverageColor(fraction)
    }
    val emptyColor = colors.surfaceOverlay

    Canvas(
        modifier
            .fillMaxWidth()
            .height(Sizing.storageBarHeight)
            .semantics {
                contentDescription = storageSpokenSummary(breakdown)
            },
    ) {
        val radius = CornerRadius(size.height / 2f)
        drawRoundRect(color = emptyColor, cornerRadius = radius)
        if (total <= 0) return@Canvas

        var x = 0f
        categories.forEachIndexed { index, category ->
            val width = (size.width * (category.bytes.toDouble() / total)).toFloat()
                .coerceAtLeast(MIN_SEGMENT_PX)
            drawRoundRect(
                color = palette.getOrElse(index) { emptyColor },
                topLeft = Offset(x, 0f),
                size = Size(width.coerceAtMost(size.width - x), size.height),
                cornerRadius = radius,
            )
            x += width
            if (x >= size.width) return@Canvas
        }
    }
}

/** The legend under the bar: one row per category with its size. */
@Composable
internal fun StorageLegend(breakdown: StorageBreakdown, modifier: Modifier = Modifier) {
    val colors = ParallaxTheme.colors
    Column(modifier.fillMaxWidth()) {
        breakdown.categories.forEachIndexed { index, category ->
            val fraction = if (breakdown.categories.size == 1) {
                0.5f
            } else {
                index.toFloat() / (breakdown.categories.size - 1)
            }
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(vertical = Spacing.xs)
                    .semantics(mergeDescendants = true) {
                        contentDescription = "${category.label}: ${formatBytes(category.bytes)}" +
                            if (category.reclaimable) ", can be freed" else ""
                    },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(
                    Modifier
                        .size(SWATCH_SIZE.dp)
                        .clip(RoundedCornerShape(Radii.xs))
                        .background(colors.coverageColor(fraction)),
                )
                Spacer(Modifier.size(Spacing.sm))
                Text(
                    text = category.label,
                    style = MaterialTheme.typography.bodyMedium,
                    color = colors.textSecondary,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    text = formatBytes(category.bytes),
                    style = MaterialTheme.typography.bodyMedium,
                    color = colors.textPrimary,
                )
            }
        }
    }
}

/** A device limitation, phrased as information rather than as an error. */
@Composable
internal fun LimitationRow(
    title: String,
    detail: String,
    affects: String,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier
            .fillMaxWidth()
            .padding(vertical = Spacing.sm)
            .semantics(mergeDescendants = true) {
                contentDescription = "$title. $detail $affects"
            },
    ) {
        Icon(
            imageVector = ParallaxIcons.Info,
            contentDescription = null,
            tint = ParallaxTheme.colors.textTertiary,
            modifier = Modifier.size(Sizing.iconSmall),
        )
        Spacer(Modifier.size(Spacing.sm))
        Column {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyMedium,
                color = ParallaxTheme.colors.textPrimary,
            )
            Text(
                text = detail,
                style = MaterialTheme.typography.bodySmall,
                color = ParallaxTheme.colors.textSecondary,
            )
            Text(
                text = affects,
                style = MaterialTheme.typography.bodySmall,
                color = ParallaxTheme.colors.textTertiary,
            )
        }
    }
}

private fun storageSpokenSummary(breakdown: StorageBreakdown): String =
    "Storage: ${formatBytes(breakdown.totalBytes)} used, " +
        "${formatBytes(breakdown.reclaimableBytes)} of it reclaimable, " +
        "${formatBytes(breakdown.deviceFreeBytes)} free on the device"

private const val MIN_SEGMENT_PX = 2f
private const val SWATCH_SIZE = 12
