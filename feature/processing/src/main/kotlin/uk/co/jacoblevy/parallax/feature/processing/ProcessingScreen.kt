package uk.co.jacoblevy.parallax.feature.processing

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import uk.co.jacoblevy.parallax.core.common.util.formatCount
import uk.co.jacoblevy.parallax.core.common.util.formatEta
import uk.co.jacoblevy.parallax.core.designsystem.component.BadgeTone
import uk.co.jacoblevy.parallax.core.designsystem.component.EmptyState
import uk.co.jacoblevy.parallax.core.designsystem.component.LabelledProgress
import uk.co.jacoblevy.parallax.core.designsystem.component.LoadingState
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxBadge
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxCard
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxIconButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxSection
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxTextButton
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Radii
import uk.co.jacoblevy.parallax.core.designsystem.theme.Sizing
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import kotlin.math.cos
import kotlin.math.sin

@Composable
fun ProcessingRoute(
    onBack: () -> Unit,
    onOpenProject: (String) -> Unit,
    viewModel: ProcessingViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    ProcessingScreen(
        state = state,
        onEvent = viewModel::onEvent,
        onBack = onBack,
        onOpenProject = onOpenProject,
    )
}

/**
 * The processing queue.
 *
 * The screen's job is to answer three questions without being asked: what is happening now, how
 * long is left, and what went wrong. Everything else -- browsing finished scans, opening a
 * model -- belongs in the library, so this stays a status screen rather than a second library.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ProcessingScreen(
    state: ProcessingUiState,
    onEvent: (ProcessingEvent) -> Unit,
    onBack: () -> Unit,
    onOpenProject: (String) -> Unit,
) {
    Scaffold(
        containerColor = ParallaxTheme.colors.surfaceSunken,
        topBar = {
            TopAppBar(
                title = { Text("Processing") },
                navigationIcon = {
                    ParallaxIconButton(
                        icon = ParallaxIcons.Back,
                        contentDescription = "Back",
                        onClick = onBack,
                    )
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = ParallaxTheme.colors.surfaceSunken,
                    titleContentColor = ParallaxTheme.colors.textPrimary,
                ),
            )
        },
    ) { padding ->
        when {
            state.loading -> LoadingState(
                modifier = Modifier.padding(padding),
                message = "Reading the queue",
            )
            state.isEmpty -> EmptyState(
                title = "Nothing to process",
                message = "Finished scans appear in your library. Queue a scan from there, " +
                    "or start a new one.",
                icon = ParallaxIcons.Queue,
                modifier = Modifier.padding(padding),
            )
            else -> LazyColumn(
                modifier = Modifier.padding(padding).fillMaxSize(),
                contentPadding = PaddingValues(
                    horizontal = Spacing.screenHorizontal,
                    vertical = Spacing.md,
                ),
                verticalArrangement = Arrangement.spacedBy(Spacing.md),
            ) {
                state.running?.let { running ->
                    item(key = "running") { RunningCard(running, onEvent) }
                }

                val waiting = state.queue.filterNot { it.isRunning }
                if (waiting.isNotEmpty()) {
                    item(key = "waiting-header") {
                        SectionHeader("Waiting", "${waiting.size} in the queue")
                    }
                    items(items = waiting, key = { it.job.id }) { entry ->
                        WaitingCard(entry, onEvent)
                    }
                }

                if (state.failed.isNotEmpty()) {
                    item(key = "failed-header") {
                        SectionHeader("Could not be processed", null)
                    }
                    items(items = state.failed, key = { "failed-${it.id}" }) { project ->
                        FailedCard(project, onEvent)
                    }
                }

                if (state.recentlyFinished.isNotEmpty()) {
                    item(key = "done-header") { SectionHeader("Finished", null) }
                    items(items = state.recentlyFinished, key = { "done-${it.id}" }) { project ->
                        FinishedCard(project, onOpenProject)
                    }
                }
            }
        }
    }
}

@Composable
private fun SectionHeader(title: String, subtitle: String?) {
    ParallaxSection(title = title, subtitle = subtitle) { Spacer(Modifier.height(Spacing.none)) }
}

/** The scan being worked on: the animated visual, the stage, the progress and the ETA. */
@Composable
private fun RunningCard(entry: QueueEntry, onEvent: (ProcessingEvent) -> Unit) {
    val status = entry.status as? ProjectStatus.Processing
    ParallaxCard(Modifier.fillMaxWidth()) {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                PointCloudSpinner()
                Spacer(Modifier.size(Spacing.md))
                Column(Modifier.weight(1f)) {
                    Text(
                        text = entry.project.name,
                        style = MaterialTheme.typography.titleMedium,
                        color = ParallaxTheme.colors.textPrimary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        text = status?.stage?.label ?: "Starting",
                        style = MaterialTheme.typography.bodySmall,
                        color = ParallaxTheme.colors.textSecondary,
                    )
                }
                ParallaxIconButton(
                    icon = ParallaxIcons.Close,
                    contentDescription = "Cancel processing ${entry.project.name}",
                    onClick = { onEvent(ProcessingEvent.Cancel(entry.project.id)) },
                )
            }
            Spacer(Modifier.height(Spacing.md))
            LabelledProgress(
                label = status?.stage?.label ?: "Preparing",
                progress = status?.overallProgress ?: 0f,
                detail = formatEta(status?.etaMillis),
            )
        }
    }
}

@Composable
private fun WaitingCard(entry: QueueEntry, onEvent: (ProcessingEvent) -> Unit) {
    ParallaxCard(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            ParallaxBadge(text = "${entry.position}", tone = BadgeTone.Neutral)
            Spacer(Modifier.size(Spacing.md))
            Column(Modifier.weight(1f)) {
                Text(
                    text = entry.project.name,
                    style = MaterialTheme.typography.titleSmall,
                    color = ParallaxTheme.colors.textPrimary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = queueDetail(entry),
                    style = MaterialTheme.typography.bodySmall,
                    color = ParallaxTheme.colors.textTertiary,
                )
            }
            ParallaxIconButton(
                icon = ParallaxIcons.Close,
                contentDescription = "Remove ${entry.project.name} from the queue",
                onClick = { onEvent(ProcessingEvent.Cancel(entry.project.id)) },
            )
        }
    }
}

@Composable
private fun FailedCard(project: Project, onEvent: (ProcessingEvent) -> Unit) {
    val failure = project.status as? ProjectStatus.Failed
    ParallaxCard(Modifier.fillMaxWidth()) {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    imageVector = ParallaxIcons.Warning,
                    contentDescription = null,
                    tint = ParallaxTheme.colors.danger,
                    modifier = Modifier.size(Sizing.iconSmall),
                )
                Spacer(Modifier.size(Spacing.sm))
                Text(
                    text = project.name,
                    style = MaterialTheme.typography.titleSmall,
                    color = ParallaxTheme.colors.textPrimary,
                    modifier = Modifier.weight(1f),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.height(Spacing.xs))
            Text(
                text = failure?.reason ?: "Processing failed.",
                style = MaterialTheme.typography.bodySmall,
                color = ParallaxTheme.colors.textSecondary,
            )
            val resumeFrom = failure?.resumableFrom
            if (resumeFrom != null) {
                Spacer(Modifier.height(Spacing.xs))
                Text(
                    // Saying where a retry starts is the difference between "try again and
                    // hope" and knowing the first twelve minutes will not be repeated.
                    text = "A retry resumes from ${resumeFrom.label.lowercase()}.",
                    style = MaterialTheme.typography.labelSmall,
                    color = ParallaxTheme.colors.textTertiary,
                )
            }
            Spacer(Modifier.height(Spacing.sm))
            Row(horizontalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                ParallaxTextButton(
                    text = "Try again",
                    onClick = { onEvent(ProcessingEvent.Retry(project.id)) },
                )
                ParallaxTextButton(
                    text = "Try at Fast quality",
                    onClick = {
                        onEvent(
                            ProcessingEvent.RetryAtQuality(
                                project.id,
                                QualityPreset.Fast,
                            ),
                        )
                    },
                )
            }
        }
    }
}

@Composable
private fun FinishedCard(project: Project, onOpen: (String) -> Unit) {
    ParallaxCard(Modifier.fillMaxWidth(), onClick = { onOpen(project.id) }) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(
                imageVector = ParallaxIcons.Check,
                contentDescription = null,
                tint = ParallaxTheme.colors.success,
                modifier = Modifier.size(Sizing.iconSmall),
            )
            Spacer(Modifier.size(Spacing.sm))
            Column(Modifier.weight(1f)) {
                Text(
                    text = project.name,
                    style = MaterialTheme.typography.titleSmall,
                    color = ParallaxTheme.colors.textPrimary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = finishedDetail(project),
                    style = MaterialTheme.typography.bodySmall,
                    color = ParallaxTheme.colors.textTertiary,
                )
            }
            Icon(
                imageVector = ParallaxIcons.ChevronRight,
                contentDescription = null,
                tint = ParallaxTheme.colors.textTertiary,
                modifier = Modifier.size(Sizing.iconSmall),
            )
        }
    }
}

/**
 * The processing visual: a point cloud forming.
 *
 * Points on a slowly rotating sphere, fading in as the rotation brings them round. It is a
 * literal picture of what the pipeline is doing -- turning views of a thing into points on its
 * surface -- which is more reassuring during a long wait than an abstract spinner, and it costs
 * one `Canvas` with a few dozen draw calls.
 */
@Composable
private fun PointCloudSpinner(modifier: Modifier = Modifier) {
    val transition = rememberInfiniteTransition(label = "pointCloud")
    val phase by transition.animateFloat(
        initialValue = 0f,
        targetValue = (2 * Math.PI).toFloat(),
        animationSpec = infiniteRepeatable(
            animation = tween(ROTATION_MILLIS, easing = LinearEasing),
            repeatMode = RepeatMode.Restart,
        ),
        label = "phase",
    )
    val accent = ParallaxTheme.colors.accent
    // A deterministic set of points on a Fibonacci sphere: evenly spread, and stable across
    // recompositions so the cloud does not reshuffle every frame.
    val points = remember {
        List(POINT_COUNT) { index ->
            val t = (index + 0.5f) / POINT_COUNT
            val inclination = kotlin.math.acos(1f - 2f * t)
            val azimuth = (GOLDEN_ANGLE * index).toFloat()
            Triple(inclination, azimuth, 0.35f + 0.65f * t)
        }
    }

    Canvas(modifier.size(Sizing.iconLarge * 1.6f)) {
        val radius = size.minDimension / 2f * 0.8f
        val centre = Offset(size.width / 2f, size.height / 2f)
        for ((inclination, azimuth, weight) in points) {
            val angle = azimuth + phase
            val x = sin(inclination.toDouble()) * cos(angle.toDouble())
            val z = sin(inclination.toDouble()) * sin(angle.toDouble())
            val y = cos(inclination.toDouble())
            // Orthographic projection, with depth driving both size and opacity so the sphere
            // reads as a volume rather than a disc.
            val depth = ((z + 1.0) / 2.0).toFloat()
            drawCircle(
                color = accent.copy(alpha = MIN_ALPHA + (1f - MIN_ALPHA) * depth * weight),
                radius = (MIN_POINT_PX + MAX_POINT_PX * depth),
                center = Offset(
                    centre.x + (x * radius).toFloat(),
                    centre.y + (y * radius).toFloat(),
                ),
            )
        }
    }
}

private fun queueDetail(entry: QueueEntry): String {
    val reason = entry.waitingReason
    val quality = entry.job.quality.label
    return if (reason == null) quality else "$quality · $reason"
}

private fun finishedDetail(project: Project): String {
    val measurements = project.measurements ?: return "Ready"
    val parts = buildList {
        if (measurements.triangleCount > 0) {
            add("${formatCount(measurements.triangleCount)} triangles")
        }
        measurements.floorAreaSquareMetres?.let { add("%.1f m² floor".format(it)) }
    }
    return if (parts.isEmpty()) "Ready" else parts.joinToString(" · ")
}

private const val ROTATION_MILLIS = 9_000
private const val POINT_COUNT = 56
private const val GOLDEN_ANGLE = 2.39996323
private const val MIN_ALPHA = 0.15f
private const val MIN_POINT_PX = 1.2f
private const val MAX_POINT_PX = 1.8f
