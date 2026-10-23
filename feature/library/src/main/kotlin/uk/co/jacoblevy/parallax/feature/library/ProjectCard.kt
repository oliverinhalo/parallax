package uk.co.jacoblevy.parallax.feature.library

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.toggleableState
import androidx.compose.ui.state.ToggleableState
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import uk.co.jacoblevy.parallax.core.common.util.formatBytes
import uk.co.jacoblevy.parallax.core.common.util.formatCount
import uk.co.jacoblevy.parallax.core.common.util.formatPercent
import uk.co.jacoblevy.parallax.core.designsystem.component.BadgeTone
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxBadge
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxProgressBar
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.motion.Motion
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Radii
import uk.co.jacoblevy.parallax.core.designsystem.theme.Sizing
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import java.io.File

/**
 * One scan in the library grid.
 *
 * The card carries its status rather than deferring it to a detail screen: in a library of
 * twenty scans, which ones are still processing and which failed is the first thing the user
 * needs, and making them tap through to find out is the difference between a tool and a toy.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun ProjectCard(
    project: Project,
    thumbnail: File?,
    folderName: String?,
    selected: Boolean,
    selectionMode: Boolean,
    onOpen: () -> Unit,
    onToggleSelected: () -> Unit,
    onToggleFavourite: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = ParallaxTheme.colors
    val shape = RoundedCornerShape(Radii.lg)
    val scale by animateFloatAsState(
        targetValue = if (selected) SELECTED_SCALE else 1f,
        animationSpec = Motion.responsiveSpring(),
        label = "cardScale",
    )
    val borderColor by animateColorAsState(
        targetValue = if (selected) colors.accent else colors.outlineSubtle,
        label = "cardBorder",
    )

    Column(
        modifier
            .scale(scale)
            .clip(shape)
            .background(colors.surfaceRaised)
            .border(if (selected) 2.dp else 1.dp, borderColor, shape)
            .combinedClickable(
                onClick = { if (selectionMode) onToggleSelected() else onOpen() },
                onLongClick = onToggleSelected,
            )
            // mergeDescendants rather than clearAndSetSemantics: the card reads as one
            // sentence, but the favourite toggle inside it stays its own focusable node.
            .semantics(mergeDescendants = true) {
                this.selected = selected
                contentDescription = spokenSummary(project, folderName)
            },
    ) {
        Box(
            Modifier
                .fillMaxWidth()
                .aspectRatio(THUMBNAIL_ASPECT),
        ) {
            ThumbnailOrPlaceholder(project, thumbnail)
            StatusOverlay(project)
            FavouriteCorner(project, onToggleFavourite)
            if (selected) {
                Box(
                    Modifier
                        .fillMaxSize()
                        .background(colors.accent.copy(alpha = SELECTED_TINT_ALPHA)),
                )
                Icon(
                    imageVector = ParallaxIcons.Check,
                    contentDescription = null,
                    tint = colors.onAccent,
                    modifier = Modifier
                        .align(Alignment.Center)
                        .size(32.dp)
                        .background(colors.accent, RoundedCornerShape(Radii.pill))
                        .padding(Spacing.xs),
                )
            }
        }

        Column(Modifier.padding(Spacing.md)) {
            Text(
                text = project.name,
                style = MaterialTheme.typography.titleSmall,
                color = colors.textPrimary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(Spacing.hairline))
            Text(
                text = subtitle(project, folderName),
                style = MaterialTheme.typography.bodySmall,
                color = colors.textTertiary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
private fun ThumbnailOrPlaceholder(project: Project, thumbnail: File?) {
    val colors = ParallaxTheme.colors
    // A placeholder rather than a decoded bitmap when there is no thumbnail yet: the gradient
    // keys off the capture mode so a grid of unprocessed scans is still distinguishable.
    val tint = when (project.captureMode) {
        CaptureMode.Object -> colors.accent
        CaptureMode.Room -> colors.success
        CaptureMode.Area -> colors.warning
        CaptureMode.Import -> colors.textTertiary
    }
    Box(
        Modifier
            .fillMaxSize()
            .background(
                Brush.linearGradient(
                    listOf(
                        colors.surfaceOverlay,
                        tint.copy(alpha = PLACEHOLDER_TINT_ALPHA),
                    ),
                ),
            ),
    ) {
        if (thumbnail != null) {
            ProjectThumbnail(file = thumbnail, modifier = Modifier.fillMaxSize())
        } else {
            Icon(
                imageVector = modeIcon(project.captureMode),
                contentDescription = null,
                tint = tint.copy(alpha = PLACEHOLDER_ICON_ALPHA),
                modifier = Modifier
                    .align(Alignment.Center)
                    .size(40.dp),
            )
        }
    }
}

/** The status chip and, while processing, the progress bar across the bottom of the thumbnail. */
@Composable
private fun StatusOverlay(project: Project) {
    val status = project.status
    Column(
        Modifier.fillMaxSize(),
        verticalArrangement = Arrangement.SpaceBetween,
    ) {
        Row(Modifier.padding(Spacing.sm)) {
            val (label, tone) = statusChip(project)
            if (label != null) ParallaxBadge(text = label, tone = tone)
        }
        if (status is ProjectStatus.Processing) {
            ParallaxProgressBar(
                progress = status.overallProgress,
                modifier = Modifier.fillMaxWidth(),
                height = 3.dp,
            )
        }
    }
}

/**
 * The favourite toggle, in the thumbnail's top-right corner.
 *
 * The glyph is 20dp but the target is a full touch target, and the semantics live on this node
 * rather than being merged into the card's single description -- a toggle the user cannot reach
 * with TalkBack is not a toggle.
 */
@Composable
private fun FavouriteCorner(project: Project, onToggle: () -> Unit) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.TopEnd) {
        Box(
            Modifier
                .size(Sizing.minTouchTarget)
                .clip(RoundedCornerShape(Radii.pill))
                .clickable(onClick = onToggle)
                .semantics {
                    role = Role.Switch
                    toggleableState = ToggleableState(project.favourite)
                    contentDescription = if (project.favourite) {
                        "Remove from favourites"
                    } else {
                        "Add to favourites"
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                imageVector = if (project.favourite) {
                    ParallaxIcons.Favourite
                } else {
                    ParallaxIcons.FavouriteOutline
                },
                contentDescription = null,
                tint = if (project.favourite) {
                    ParallaxTheme.colors.accent
                } else {
                    Color.White.copy(alpha = UNSET_FAVOURITE_ALPHA)
                },
                modifier = Modifier.size(20.dp),
            )
        }
    }
}

private fun modeIcon(mode: CaptureMode) = when (mode) {
    CaptureMode.Object -> ParallaxIcons.ModeObject
    CaptureMode.Room -> ParallaxIcons.ModeRoom
    CaptureMode.Area -> ParallaxIcons.ModeArea
    CaptureMode.Import -> ParallaxIcons.ModeImport
}

/** The chip shown over the thumbnail, or null when the scan is simply ready. */
private fun statusChip(project: Project): Pair<String?, BadgeTone> = when (val s = project.status) {
    is ProjectStatus.Capturing -> "Unfinished" to BadgeTone.Warning
    ProjectStatus.Captured -> "Not processed" to BadgeTone.Neutral
    is ProjectStatus.Queued -> "Queued ${s.position}" to BadgeTone.Neutral
    is ProjectStatus.Processing -> formatPercent(s.overallProgress) to BadgeTone.Accent
    is ProjectStatus.Ready -> if (s.refiningSplats) {
        "Refining" to BadgeTone.Accent
    } else {
        null to BadgeTone.Neutral
    }
    is ProjectStatus.Failed -> "Failed" to BadgeTone.Danger
    ProjectStatus.Cancelled -> "Cancelled" to BadgeTone.Neutral
}

private fun subtitle(project: Project, folderName: String?): String {
    val parts = buildList {
        add(modeLabel(project.captureMode))
        if (project.storageBytes > 0) add(formatBytes(project.storageBytes))
        project.measurements?.triangleCount?.takeIf { it > 0 }?.let { add("${formatCount(it)} tris") }
        folderName?.let { add(it) }
    }
    return parts.joinToString(" · ")
}

private fun modeLabel(mode: CaptureMode) = when (mode) {
    CaptureMode.Object -> "Object"
    CaptureMode.Room -> "Room"
    CaptureMode.Area -> "Area"
    CaptureMode.Import -> "Import"
}

/**
 * What TalkBack reads for the whole card.
 *
 * One node, not six: hearing "Hallway, Room, 91 MB, heart, 72 percent, button" is far worse
 * than one sentence that says what this scan is and what state it is in.
 */
private fun spokenSummary(project: Project, folderName: String?): String {
    val state = when (val s = project.status) {
        is ProjectStatus.Capturing -> "capture unfinished, ${s.frameCount} frames"
        ProjectStatus.Captured -> "not processed yet"
        is ProjectStatus.Queued -> "queued, position ${s.position}"
        is ProjectStatus.Processing ->
            "processing, ${formatPercent(s.overallProgress)}, ${s.stage.label}"
        is ProjectStatus.Ready -> if (s.refiningSplats) "ready, still refining" else "ready"
        is ProjectStatus.Failed -> "failed at ${s.stage.label}"
        ProjectStatus.Cancelled -> "cancelled"
    }
    return buildList {
        add(project.name)
        add(modeLabel(project.captureMode) + " scan")
        add(state)
        if (project.favourite) add("favourite")
        folderName?.let { add("in $it") }
    }.joinToString(", ")
}

private const val THUMBNAIL_ASPECT = 4f / 3f
private const val SELECTED_SCALE = 0.97f
private const val SELECTED_TINT_ALPHA = 0.22f
private const val PLACEHOLDER_TINT_ALPHA = 0.18f
private const val PLACEHOLDER_ICON_ALPHA = 0.5f
private const val UNSET_FAVOURITE_ALPHA = 0.55f
