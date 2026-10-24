package uk.co.jacoblevy.parallax.feature.viewer.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.IntSize
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import uk.co.jacoblevy.parallax.core.designsystem.component.EmptyState
import uk.co.jacoblevy.parallax.core.designsystem.component.GlassPanel
import uk.co.jacoblevy.parallax.core.designsystem.component.LoadingState
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxChip
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxIconButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxTextButton
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.LightingPreset
import uk.co.jacoblevy.parallax.feature.viewer.ViewerInteraction
import uk.co.jacoblevy.parallax.feature.viewer.ViewerUiState
import uk.co.jacoblevy.parallax.feature.viewer.ViewerViewModel
import uk.co.jacoblevy.parallax.feature.viewer.measure.MeasureTool

/**
 * The viewer.
 *
 * One rendering surface with everything else floating over it on glass, so the
 * model is never squeezed into a panel. The controls collapse to a single row
 * when a tool is active, because the point of a measuring tool is the model
 * underneath it.
 */
@Composable
fun ViewerScreen(
    onBack: () -> Unit,
    onExport: (String) -> Unit,
    onPlaceInRoom: (String) -> Unit,
    modifier: Modifier = Modifier,
    viewModel: ViewerViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    var viewport by remember { mutableStateOf(IntSize.Zero) }
    var showSettings by remember { mutableStateOf(false) }

    Box(modifier.fillMaxSize()) {
        when {
            state.loading -> LoadingState(Modifier.fillMaxSize(), "Opening the scan")
            state.project == null -> EmptyState(
                modifier = Modifier.fillMaxSize(),
                icon = ParallaxIcons.Warning,
                title = "This scan is no longer here",
                message = state.message ?: "It may have been deleted from another screen.",
            )
            state.rendererFailed -> RendererUnavailable(onBack)
            else -> {
                ModelView(
                    state = state,
                    onRendererFailed = viewModel::rendererFailed,
                    onFrame = viewModel::advanceCamera,
                    onViewportChanged = { width, height -> viewport = IntSize(width, height) },
                    modifier = Modifier
                        .fillMaxSize()
                        .viewerGestures(state, viewport, viewModel),
                )
                ViewerOverlay(
                    state = state,
                    viewModel = viewModel,
                    showSettings = showSettings,
                    onToggleSettings = { showSettings = !showSettings },
                    onBack = onBack,
                    onExport = { state.project?.let { onExport(it.id) } },
                    onPlaceInRoom = { state.project?.let { onPlaceInRoom(it.id) } },
                )
            }
        }
    }
}

/**
 * Drag, pinch and tap.
 *
 * Both gesture detectors on the same node, with the transform one first:
 * Compose gives the first claimant the pointer, and a measuring tap has to be
 * able to lose to a drag or every attempt to rotate the model drops a point.
 */
private fun Modifier.viewerGestures(
    state: ViewerUiState,
    viewport: IntSize,
    viewModel: ViewerViewModel,
): Modifier = this
    .pointerInput(state.walking, state.interaction) {
        detectTransformGestures { _, pan, zoom, _ ->
            if (zoom != 1f) viewModel.zoom(zoom)
            if (pan != androidx.compose.ui.geometry.Offset.Zero) {
                val width = size.width.coerceAtLeast(1)
                val height = size.height.coerceAtLeast(1)
                if (state.walking) {
                    viewModel.rotate(
                        -pan.x / width * WALK_LOOK_RADIANS,
                        pan.y / height * WALK_LOOK_RADIANS,
                    )
                } else if (state.interaction == ViewerInteraction.Look) {
                    viewModel.rotate(
                        -pan.x / width * ORBIT_RADIANS,
                        pan.y / height * ORBIT_RADIANS,
                    )
                } else {
                    // With a tool active, dragging pans instead of rotating:
                    // the user is lining up a point, not inspecting the model.
                    viewModel.pan(pan.x / width, pan.y / height)
                }
            }
        }
    }
    .pointerInput(state.interaction, viewport) {
        detectTapGestures { offset ->
            viewModel.tap(offset.x, offset.y, viewport.width, viewport.height)
        }
    }

@Composable
private fun RendererUnavailable(onBack: () -> Unit) {
    // The model is still here and still exportable; only the preview is gone.
    // Saying that is far better than a blank screen or a crash.
    EmptyState(
        modifier = Modifier.fillMaxSize(),
        icon = ParallaxIcons.Warning,
        title = "3D preview unavailable",
        message = "This device's graphics driver would not start a 3D view. The scan itself " +
            "is fine — you can still export it and read its measurements.",
        actionLabel = "Back to library",
        onAction = onBack,
    )
}

@Composable
private fun ViewerOverlay(
    state: ViewerUiState,
    viewModel: ViewerViewModel,
    showSettings: Boolean,
    onToggleSettings: () -> Unit,
    onBack: () -> Unit,
    onExport: () -> Unit,
    onPlaceInRoom: () -> Unit,
) {
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(horizontal = Spacing.screenHorizontal, vertical = Spacing.sm),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ParallaxIconButton(
                icon = ParallaxIcons.Back,
                contentDescription = "Back",
                onClick = onBack,
            )
            Spacer(Modifier.width(Spacing.sm))
            GlassPanel(contentPadding = androidx.compose.foundation.layout.PaddingValues(
                horizontal = Spacing.md,
                vertical = Spacing.sm,
            )) {
                Text(
                    text = state.project?.name.orEmpty(),
                    style = MaterialTheme.typography.titleSmall,
                    color = ParallaxTheme.colors.textPrimary,
                )
            }
            Spacer(Modifier.weight(1f))
            ParallaxIconButton(
                icon = ParallaxIcons.Lighting,
                contentDescription = if (showSettings) "Hide view settings" else "View settings",
                onClick = onToggleSettings,
            )
        }

        AnimatedVisibility(visible = showSettings) {
            ViewSettingsPanel(state, viewModel)
        }

        Spacer(Modifier.weight(1f))

        state.message?.let { message ->
            GlassPanel(
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = Spacing.screenHorizontal, vertical = Spacing.sm),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = message,
                        style = MaterialTheme.typography.bodyMedium,
                        color = ParallaxTheme.colors.textPrimary,
                        modifier = Modifier.weight(1f),
                    )
                    ParallaxTextButton(text = "Dismiss", onClick = viewModel::dismissMessage)
                }
            }
        }

        if (state.interaction == ViewerInteraction.Measure) {
            MeasurePanel(state, viewModel)
        }
        if (state.pendingAnnotation != null) AnnotationComposer(viewModel)
        if (state.walking) WalkControls(state, viewModel)

        ToolBar(
            state = state,
            viewModel = viewModel,
            onExport = onExport,
            onPlaceInRoom = onPlaceInRoom,
            modifier = Modifier
                .fillMaxWidth()
                .navigationBarsPadding()
                .padding(horizontal = Spacing.screenHorizontal, vertical = Spacing.md),
        )
    }
}

@Composable
private fun ViewSettingsPanel(state: ViewerUiState, viewModel: ViewerViewModel) {
    GlassPanel(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = Spacing.screenHorizontal)
            .testTag("viewSettings"),
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(Spacing.md)) {
            Text(
                "Shown as",
                style = MaterialTheme.typography.labelLarge,
                color = ParallaxTheme.colors.textSecondary,
            )
            Row(
                Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
            ) {
                for (mode in state.availableModes) {
                    ParallaxChip(
                        text = mode.label,
                        selected = mode == state.renderMode,
                        onClick = { viewModel.setRenderMode(mode) },
                    )
                }
            }

            Text(
                "Lighting",
                style = MaterialTheme.typography.labelLarge,
                color = ParallaxTheme.colors.textSecondary,
            )
            Row(
                Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
            ) {
                for (preset in LightingPreset.entries) {
                    ParallaxChip(
                        text = preset.label,
                        selected = preset == state.lighting,
                        onClick = { viewModel.setLighting(preset) },
                    )
                }
            }

            Text(
                "Brightness",
                style = MaterialTheme.typography.labelLarge,
                color = ParallaxTheme.colors.textSecondary,
            )
            Slider(
                value = state.exposureStops,
                onValueChange = viewModel::setExposure,
                valueRange = -2f..2f,
                modifier = Modifier.semantics {
                    contentDescription = "Brightness, ${state.exposureStops} stops"
                },
            )
        }
    }
}

@Composable
private fun WalkControls(state: ViewerUiState, viewModel: ViewerViewModel) {
    GlassPanel(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = Spacing.screenHorizontal, vertical = Spacing.sm),
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
            Row(horizontalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                ParallaxTextButton(
                    text = "Forward",
                    onClick = { viewModel.walk(WALK_STEP_METRES, 0f) },
                )
                ParallaxTextButton(
                    text = "Back",
                    onClick = { viewModel.walk(-WALK_STEP_METRES, 0f) },
                )
                ParallaxTextButton(
                    text = "Left",
                    onClick = { viewModel.walk(0f, -WALK_STEP_METRES) },
                )
                ParallaxTextButton(
                    text = "Right",
                    onClick = { viewModel.walk(0f, WALK_STEP_METRES) },
                )
            }
            Text(
                "Eye height",
                style = MaterialTheme.typography.labelLarge,
                color = ParallaxTheme.colors.textSecondary,
            )
            Slider(
                value = state.walk.eyeHeightMetres,
                onValueChange = viewModel::setEyeHeight,
                valueRange = 0.4f..2.2f,
                modifier = Modifier.semantics {
                    contentDescription = "Eye height, ${state.walk.eyeHeightMetres} metres"
                },
            )
        }
    }
}

@Composable
private fun ToolBar(
    state: ViewerUiState,
    viewModel: ViewerViewModel,
    onExport: () -> Unit,
    onPlaceInRoom: () -> Unit,
    modifier: Modifier = Modifier,
) {
    GlassPanel(modifier) {
        Row(
            Modifier.horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ParallaxChip(
                text = "Look",
                selected = state.interaction == ViewerInteraction.Look,
                onClick = { viewModel.setInteraction(ViewerInteraction.Look) },
            )
            ParallaxChip(
                text = "Measure",
                selected = state.interaction == ViewerInteraction.Measure,
                onClick = { viewModel.setInteraction(ViewerInteraction.Measure) },
            )
            ParallaxChip(
                text = "Note",
                selected = state.interaction == ViewerInteraction.Annotate,
                onClick = { viewModel.setInteraction(ViewerInteraction.Annotate) },
            )
            ParallaxChip(
                text = "Walk",
                selected = state.walking,
                onClick = { viewModel.setWalking(!state.walking) },
            )
            ParallaxChip(text = "Reset", selected = false, onClick = viewModel::resetView)
            ParallaxChip(text = "Place in room", selected = false, onClick = onPlaceInRoom)
            ParallaxChip(text = "Export", selected = false, onClick = onExport)
        }
    }
}

private const val ORBIT_RADIANS = 3.2f
private const val WALK_LOOK_RADIANS = 2.2f
private const val WALK_STEP_METRES = 0.35f

/** Entry point for the navigation graph. */
@Composable
fun ViewerRoute(
    onBack: () -> Unit,
    onExport: (String) -> Unit,
    onPlaceInRoom: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    ViewerScreen(
        onBack = onBack,
        onExport = onExport,
        onPlaceInRoom = onPlaceInRoom,
        modifier = modifier,
    )
}
