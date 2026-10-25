package uk.co.jacoblevy.parallax.feature.capture.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import uk.co.jacoblevy.parallax.core.common.util.formatBytes
import uk.co.jacoblevy.parallax.core.common.util.formatCount
import uk.co.jacoblevy.parallax.core.common.util.formatDuration
import uk.co.jacoblevy.parallax.core.common.util.formatPercent
import uk.co.jacoblevy.parallax.core.designsystem.component.BadgeTone
import uk.co.jacoblevy.parallax.core.designsystem.component.CompactStat
import uk.co.jacoblevy.parallax.core.designsystem.component.GlassPanel
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxChip
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxIconButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxProgressBar
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxTextButton
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Radii
import uk.co.jacoblevy.parallax.core.designsystem.theme.Sizing
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.GuidancePrompt
import uk.co.jacoblevy.parallax.core.model.TrackingState
import uk.co.jacoblevy.parallax.core.model.HapticCue
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.feature.capture.CaptureEvent
import uk.co.jacoblevy.parallax.feature.capture.CapturePhase
import uk.co.jacoblevy.parallax.feature.capture.CaptureUiState
import uk.co.jacoblevy.parallax.feature.capture.CaptureViewModel
import uk.co.jacoblevy.parallax.feature.capture.session.ArSessionFailure
import uk.co.jacoblevy.parallax.feature.capture.session.CaptureEngine
import uk.co.jacoblevy.parallax.feature.capture.session.CaptureTick

@Composable
fun CaptureRoute(
    onFinished: (String) -> Unit,
    onCancelled: () -> Unit,
    viewModel: CaptureViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()

    LaunchedEffect(state.finishedProjectId) {
        state.finishedProjectId?.let(onFinished)
    }
    DisposableEffect(Unit) { onDispose { viewModel.onStopped() } }

    CaptureScreen(
        state = state,
        onEvent = viewModel::onEvent,
        onCancelled = onCancelled,
        engineProvider = { viewModel.engine },
        onTick = viewModel::onFrameProcessed,
        onSessionFailure = viewModel::onSessionFailure,
    )
}

/**
 * The capture screen.
 *
 * Minimal chrome over a live camera feed, with every control inside thumb reach at the bottom.
 * The design direction's one-handed requirement is the constraint that shapes this: the shutter
 * is large and low, the mode switcher sits just above it, and the readouts that matter while
 * moving -- coverage, prompt -- are at the top where a hand does not cover them.
 */
@Composable
internal fun CaptureScreen(
    state: CaptureUiState,
    onEvent: (CaptureEvent) -> Unit,
    onCancelled: () -> Unit,
    engineProvider: () -> CaptureEngine?,
    onTick: (CaptureTick) -> Unit,
    onSessionFailure: (ArSessionFailure, String?) -> Unit,
) {
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        if (!state.blockedByArCore && state.mode.requiresArCore) {
            ArViewfinder(
                preferDepth = state.capabilities.supportsDepthApi,
                recording = state.isRecording,
                engineProvider = engineProvider,
                onTick = onTick,
                onFailure = onSessionFailure,
                modifier = Modifier.fillMaxSize(),
            )
        }

        // A scrim top and bottom so white text stays legible over a bright scene without
        // dimming the middle of the frame, which is what the user is actually aiming.
        Box(
            Modifier
                .fillMaxSize()
                .background(
                    Brush.verticalGradient(
                        0f to ParallaxTheme.colors.cameraScrim,
                        SCRIM_FADE_TOP to Color.Transparent,
                        SCRIM_FADE_BOTTOM to Color.Transparent,
                        1f to ParallaxTheme.colors.cameraScrim,
                    ),
                ),
        )

        TopBar(state, onCancelled, Modifier.align(Alignment.TopCenter))
        PromptBanner(state.prompt, Modifier.align(Alignment.TopCenter).padding(top = PROMPT_TOP.dp))

        when {
            state.mode == CaptureMode.Import -> ImportPanel(
                state = state,
                onEvent = onEvent,
                modifier = Modifier.align(Alignment.Center),
            )
            state.sessionFailure != null -> SessionFailurePanel(
                state = state,
                onEvent = onEvent,
                onCancelled = onCancelled,
                modifier = Modifier.align(Alignment.Center),
            )
            state.blockedByArCore -> ArUnavailablePanel(
                state = state,
                onEvent = onEvent,
                modifier = Modifier.align(Alignment.Center),
            )
        }

        Controls(
            state = state,
            onEvent = onEvent,
            modifier = Modifier.align(Alignment.BottomCenter),
        )
    }
}

@Composable
private fun TopBar(state: CaptureUiState, onCancelled: () -> Unit, modifier: Modifier) {
    Row(
        modifier
            .fillMaxWidth()
            .statusBarsPadding()
            .padding(horizontal = Spacing.md),
        verticalAlignment = Alignment.Top,
    ) {
        ParallaxIconButton(
            icon = ParallaxIcons.Close,
            contentDescription = if (state.isRecording) "Discard this scan" else "Close",
            onClick = onCancelled,
        )
        Spacer(Modifier.width(Spacing.sm))
        if (state.phase != CapturePhase.Ready) {
            GlassPanel(cornerRadius = Radii.md) {
                Row(horizontalArrangement = Arrangement.spacedBy(Spacing.md)) {
                    CompactStat(
                        label = "FRAMES",
                        value = formatCount(state.stats.keyframeCount.toLong()),
                    )
                    CompactStat(
                        label = "COVER",
                        value = formatPercent(state.stats.coverage),
                        tone = if (state.stats.coverage > COVERAGE_GOOD) {
                            BadgeTone.Success
                        } else {
                            BadgeTone.Neutral
                        },
                    )
                    CompactStat(label = "TIME", value = formatDuration(state.stats.elapsedMillis))
                    if (state.showAdvancedStats) {
                        CompactStat(
                            label = "RATE",
                            value = "%.1f/s".format(state.stats.captureRateHz),
                        )
                        CompactStat(
                            label = "REJECT",
                            value = formatPercent(state.stats.rejectionRate),
                        )
                    }
                }
            }
        }
        Spacer(Modifier.weight(1f))
        Column(horizontalAlignment = Alignment.End) {
            if (state.phase != CapturePhase.Ready) {
                // Larger while recording. It is the only picture of what has actually been
                // captured, and at the resting size it was a decoration rather than something
                // a user could steer by.
                CoverageDome(
                    coverage = state.coverage,
                    size = if (state.isRecording) {
                        Sizing.coverageDomeSizeLarge
                    } else {
                        Sizing.coverageDomeSize
                    },
                )
                Spacer(Modifier.height(Spacing.sm))
            }
            WarningBadges(state)
        }
    }
}

/** Thermal, battery and storage, shown only when they are worth saying. */
@Composable
private fun WarningBadges(state: CaptureUiState) {
    val stats = state.stats
    Column(
        horizontalAlignment = Alignment.End,
        verticalArrangement = Arrangement.spacedBy(Spacing.xs),
    ) {
        if (stats.thermalStatus.shouldWarn) {
            GlassPanel(cornerRadius = Radii.pill) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = ParallaxIcons.Thermal,
                        contentDescription = null,
                        tint = ParallaxTheme.colors.warning,
                        modifier = Modifier.size(Sizing.iconSmall),
                    )
                    Spacer(Modifier.width(Spacing.xs))
                    Text(
                        text = "Warm",
                        style = MaterialTheme.typography.labelSmall,
                        color = Color.White,
                    )
                }
            }
        }
        if (stats.batteryPercent in 1..LOW_BATTERY) {
            GlassPanel(cornerRadius = Radii.pill) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = ParallaxIcons.Battery,
                        contentDescription = null,
                        tint = ParallaxTheme.colors.danger,
                        modifier = Modifier.size(Sizing.iconSmall),
                    )
                    Spacer(Modifier.width(Spacing.xs))
                    Text(
                        text = "${stats.batteryPercent}%",
                        style = MaterialTheme.typography.labelSmall,
                        color = Color.White,
                    )
                }
            }
        }
        if (state.phase != CapturePhase.Ready) {
            GlassPanel(cornerRadius = Radii.pill) {
                Text(
                    text = formatBytes(stats.storageUsedBytes),
                    style = MaterialTheme.typography.labelSmall,
                    color = Color.White,
                )
            }
        }
    }
}

/**
 * What capture is actually doing, in one line.
 *
 * Kept against seen, and the reason the rest were not kept. A scan that produces nothing used
 * to look identical to one going perfectly -- the counter was the only difference and it was
 * behind a setting -- so this is always visible while recording. It is also the line the
 * "no frames are being kept" prompt tells the user to look at, which it can only do if it is
 * on screen without being switched on first.
 */
@Composable
private fun CaptureReadout(state: CaptureUiState) {
    val stats = state.stats
    val tracking = stats.trackingState
    val reason = when {
        tracking != TrackingState.Tracking -> tracking.label
        stats.keyframeCount == 0 && stats.frameCount == 0 -> "waiting for the camera"
        else -> null
    }

    GlassPanel(cornerRadius = Radii.pill) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = "${stats.keyframeCount} kept",
                style = MaterialTheme.typography.labelSmall,
                color = Color.White,
            )
            Spacer(Modifier.width(Spacing.xs))
            Text(
                text = "of ${stats.frameCount}",
                style = MaterialTheme.typography.labelSmall,
                color = Color.White.copy(alpha = MUTED_ALPHA),
            )
            if (reason != null) {
                Spacer(Modifier.width(Spacing.xs))
                Text(
                    text = "· $reason",
                    style = MaterialTheme.typography.labelSmall,
                    color = ParallaxTheme.colors.warning,
                )
            }
        }
    }
}

/**
 * The guidance prompt.
 *
 * Animated in and out, and never more than one at a time: the engine picks the single
 * highest-priority condition, and the session holds it visible long enough to read.
 */
@Composable
private fun PromptBanner(prompt: GuidancePrompt?, modifier: Modifier) {
    AnimatedVisibility(
        visible = prompt != null,
        enter = slideInVertically { -it } + fadeIn(),
        exit = slideOutVertically { -it } + fadeOut(),
        modifier = modifier,
    ) {
        val current = prompt ?: return@AnimatedVisibility
        val tone = when (current.haptic) {
            HapticCue.Warning -> ParallaxTheme.colors.warning
            HapticCue.Success -> ParallaxTheme.colors.success
            else -> ParallaxTheme.colors.accent
        }
        GlassPanel(cornerRadius = Radii.pill) {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.semantics {
                    contentDescription = "${current.message}. ${current.detail}"
                },
            ) {
                Text(
                    text = current.message,
                    style = MaterialTheme.typography.titleSmall,
                    color = tone,
                    textAlign = TextAlign.Center,
                )
                Text(
                    text = current.detail,
                    style = MaterialTheme.typography.bodySmall,
                    color = Color.White.copy(alpha = DETAIL_ALPHA),
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

@Composable
private fun Controls(state: CaptureUiState, onEvent: (CaptureEvent) -> Unit, modifier: Modifier) {
    Column(
        modifier
            .fillMaxWidth()
            .systemBarsPadding()
            .padding(bottom = Spacing.lg),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        if (state.mode == CaptureMode.Import) {
            // Import has no shutter -- the panel in the middle of the screen owns the action --
            // but the quality preset still matters, because it caps how many frames are kept.
            QualitySwitcher(state, onEvent)
            Spacer(Modifier.height(Spacing.md))
            ModeSwitcher(state, onEvent)
            return@Column
        }

        if (state.phase == CapturePhase.Ready) {
            ModeSwitcher(state, onEvent)
            Spacer(Modifier.height(Spacing.md))
            QualitySwitcher(state, onEvent)
            Spacer(Modifier.height(Spacing.lg))
        }

        if (state.isRecording) {
            MotionMeter(
                band = state.stats.motion,
                modifier = Modifier.width(MotionMeterWidth),
            )
            Spacer(Modifier.height(Spacing.xs))
            CaptureReadout(state)
            Spacer(Modifier.height(Spacing.md))
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            if (state.phase == CapturePhase.Capturing || state.phase == CapturePhase.Paused) {
                ParallaxIconButton(
                    icon = if (state.isRecording) ParallaxIcons.Pause else ParallaxIcons.Play,
                    contentDescription = if (state.isRecording) "Pause" else "Resume",
                    onClick = {
                        onEvent(
                            if (state.isRecording) CaptureEvent.Pause else CaptureEvent.Resume,
                        )
                    },
                )
                Spacer(Modifier.width(Spacing.xl))
            }

            Shutter(state = state, onEvent = onEvent)

            if (state.phase == CapturePhase.Capturing || state.phase == CapturePhase.Paused) {
                Spacer(Modifier.width(Spacing.xl))
                ParallaxIconButton(
                    icon = ParallaxIcons.Check,
                    contentDescription = if (state.canFinish) {
                        "Finish and process"
                    } else {
                        "Not enough frames yet"
                    },
                    enabled = state.canFinish,
                    selected = state.canFinish,
                    onClick = { onEvent(CaptureEvent.Finish) },
                )
            }
        }

        if (state.phase == CapturePhase.Ready) {
            Spacer(Modifier.height(Spacing.sm))
            Text(
                text = readyHint(state.mode),
                style = MaterialTheme.typography.bodySmall,
                color = Color.White.copy(alpha = DETAIL_ALPHA),
                textAlign = TextAlign.Center,
                modifier = Modifier.padding(horizontal = Spacing.xl),
            )
        }
    }
}

/**
 * The shutter.
 *
 * Deliberately large, and it pulses while recording. On a scanner the shutter is not a
 * photograph button -- it starts and stops a session -- so it changes shape rather than just
 * colour: a ring while idle, a filled square while recording.
 */
@Composable
private fun Shutter(state: CaptureUiState, onEvent: (CaptureEvent) -> Unit) {
    val recording = state.phase == CapturePhase.Capturing
    val pulse by animateFloatAsState(
        targetValue = if (recording) SHUTTER_RECORDING_SCALE else 1f,
        label = "shutterPulse",
    )
    val label = when (state.phase) {
        CapturePhase.Ready -> "Start scanning"
        CapturePhase.Capturing -> "Stop scanning"
        CapturePhase.Paused -> "Stop scanning"
        CapturePhase.Finished -> "Finishing"
    }
    Box(
        Modifier
            .size(Sizing.shutterOuter)
            .scale(pulse)
            .clip(CircleShape)
            .border(SHUTTER_RING.dp, Color.White, CircleShape)
            .clickable(enabled = state.phase != CapturePhase.Finished) {
                when (state.phase) {
                    CapturePhase.Ready -> onEvent(CaptureEvent.Start)
                    CapturePhase.Capturing, CapturePhase.Paused ->
                        if (state.canFinish) {
                            onEvent(CaptureEvent.Finish)
                        } else {
                            onEvent(CaptureEvent.Discard)
                        }
                    CapturePhase.Finished -> Unit
                }
            }
            .semantics { contentDescription = label },
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier
                .size(Sizing.shutterInner)
                .clip(if (recording) RoundedCornerShape(Radii.sm) else CircleShape)
                .background(
                    if (recording) {
                        ParallaxTheme.colors.danger
                    } else {
                        ParallaxTheme.colors.accent
                    },
                ),
        )
    }
}

@Composable
private fun ModeSwitcher(state: CaptureUiState, onEvent: (CaptureEvent) -> Unit) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
        modifier = Modifier.padding(horizontal = Spacing.md),
    ) {
        CaptureMode.entries.forEach { mode ->
            val available = mode in state.availableModes
            ParallaxChip(
                text = modeLabel(mode),
                selected = state.mode == mode,
                enabled = available,
                icon = modeIcon(mode),
                onClick = { onEvent(CaptureEvent.ModeSelected(mode)) },
            )
        }
    }
}

@Composable
private fun QualitySwitcher(state: CaptureUiState, onEvent: (CaptureEvent) -> Unit) {
    Row(horizontalArrangement = Arrangement.spacedBy(Spacing.sm)) {
        QualityPreset.entries.forEach { preset ->
            ParallaxChip(
                text = preset.label,
                selected = state.quality == preset,
                // A preset the device cannot run is shown but not selectable, so the ceiling is
                // visible rather than the option silently missing.
                enabled = preset.ordinal <= state.capabilities.maxQualityPreset.ordinal,
                onClick = { onEvent(CaptureEvent.QualitySelected(preset)) },
            )
        }
    }
}

/**
 * Import mode's panel.
 *
 * Import has no viewfinder: the frames already exist. The panel explains what makes a set
 * usable, because that is the one thing the user controls and the most common reason an import
 * reconstructs badly.
 */
@Composable
private fun ImportPanel(
    state: CaptureUiState,
    onEvent: (CaptureEvent) -> Unit,
    modifier: Modifier,
) {
    val picker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.PickMultipleVisualMedia(MAX_IMPORT_ITEMS),
    ) { uris -> if (uris.isNotEmpty()) onEvent(CaptureEvent.ImportMedia(uris)) }

    GlassPanel(modifier.padding(Spacing.xl), cornerRadius = Radii.lg) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(
                imageVector = ParallaxIcons.ModeImport,
                contentDescription = null,
                tint = ParallaxTheme.colors.accent,
                modifier = Modifier.size(Sizing.iconLarge),
            )
            Spacer(Modifier.height(Spacing.md))
            Text(
                text = "Build from photos or video",
                style = MaterialTheme.typography.titleMedium,
                color = Color.White,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(Spacing.sm))
            Text(
                text = "Pick a set taken while walking around the subject. Photos from one " +
                    "spot cannot be reconstructed, however many there are: depth comes from " +
                    "the camera moving.",
                style = MaterialTheme.typography.bodySmall,
                color = Color.White.copy(alpha = DETAIL_ALPHA),
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(Spacing.lg))

            val progress = state.importProgress
            if (progress != null) {
                Text(
                    text = progress.label,
                    style = MaterialTheme.typography.bodyMedium,
                    color = Color.White,
                )
                Spacer(Modifier.height(Spacing.sm))
                ParallaxProgressBar(progress = progress.fraction)
                Spacer(Modifier.height(Spacing.xs))
                Text(
                    text = "${progress.accepted} frames kept",
                    style = MaterialTheme.typography.labelSmall,
                    color = Color.White.copy(alpha = DETAIL_ALPHA),
                )
            } else {
                ParallaxButton(
                    text = "Choose media",
                    onClick = {
                        picker.launch(
                            PickVisualMediaRequest(
                                ActivityResultContracts.PickVisualMedia.ImageAndVideo,
                            ),
                        )
                    },
                    icon = ParallaxIcons.ModeImport,
                )
            }
        }
    }
}

@Composable
private fun SessionFailurePanel(
    state: CaptureUiState,
    onEvent: (CaptureEvent) -> Unit,
    onCancelled: () -> Unit,
    modifier: Modifier,
) {
    val failure = state.sessionFailure ?: return
    GlassPanel(modifier.padding(Spacing.xl), cornerRadius = Radii.lg) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = failure.message,
                style = MaterialTheme.typography.titleMedium,
                color = Color.White,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(Spacing.sm))
            Text(
                text = failure.userAction,
                style = MaterialTheme.typography.bodyMedium,
                color = Color.White.copy(alpha = DETAIL_ALPHA),
                textAlign = TextAlign.Center,
            )
            // Only for the failures that have no explanation of their own. For "ARCore needs
            // updating" the user action is the whole story and a class name underneath it
            // would be noise; for "stopped unexpectedly" it is the only story there is.
            state.sessionFailureDetail?.let { detail ->
                Spacer(Modifier.height(Spacing.sm))
                Text(
                    text = detail,
                    style = MaterialTheme.typography.labelSmall,
                    color = Color.White.copy(alpha = DETAIL_ALPHA),
                    textAlign = TextAlign.Center,
                )
            }
            Spacer(Modifier.height(Spacing.lg))
            Row(horizontalArrangement = Arrangement.spacedBy(Spacing.md)) {
                ParallaxTextButton(
                    text = "Back",
                    onClick = onCancelled,
                )
                ParallaxButton(
                    text = "Try again",
                    onClick = { onEvent(CaptureEvent.DismissError) },
                    icon = ParallaxIcons.Retry,
                )
            }
        }
    }
}

@Composable
private fun ArUnavailablePanel(
    state: CaptureUiState,
    onEvent: (CaptureEvent) -> Unit,
    modifier: Modifier,
) {
    GlassPanel(modifier.padding(Spacing.xl), cornerRadius = Radii.lg) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = "${modeLabel(state.mode)} mode needs AR",
                style = MaterialTheme.typography.titleMedium,
                color = Color.White,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(Spacing.sm))
            Text(
                text = state.capabilities.arCoreAvailability.explanation,
                style = MaterialTheme.typography.bodyMedium,
                color = Color.White.copy(alpha = DETAIL_ALPHA),
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(Spacing.lg))
            ParallaxButton(
                text = "Use Import instead",
                onClick = { onEvent(CaptureEvent.ModeSelected(CaptureMode.Import)) },
                icon = ParallaxIcons.ModeImport,
            )
        }
    }
}

private fun modeLabel(mode: CaptureMode) = when (mode) {
    CaptureMode.Object -> "Object"
    CaptureMode.Room -> "Room"
    CaptureMode.Area -> "Area"
    CaptureMode.Import -> "Import"
}

private fun modeIcon(mode: CaptureMode) = when (mode) {
    CaptureMode.Object -> ParallaxIcons.ModeObject
    CaptureMode.Room -> ParallaxIcons.ModeRoom
    CaptureMode.Area -> ParallaxIcons.ModeArea
    CaptureMode.Import -> ParallaxIcons.ModeImport
}

private fun readyHint(mode: CaptureMode) = when (mode) {
    CaptureMode.Object ->
        "Walk all the way round, at three different heights. The dome shows what is missing."
    CaptureMode.Room ->
        "Walk the perimeter slowly, pointing at the walls, then sweep the floor and ceiling."
    CaptureMode.Area ->
        "Cover the space in overlapping passes. Longer scans are processed in chunks."
    CaptureMode.Import ->
        "Pick photos or a video from your gallery. No AR needed."
}

private const val SCRIM_FADE_TOP = 0.22f
private const val SCRIM_FADE_BOTTOM = 0.62f
private const val PROMPT_TOP = 96
private const val DETAIL_ALPHA = 0.75f
private const val COVERAGE_GOOD = 0.6f
private const val MUTED_ALPHA = 0.6f
private const val LOW_BATTERY = 20
private const val SHUTTER_RECORDING_SCALE = 1.06f
private const val SHUTTER_RING = 3

/** The photo picker's selection cap. Beyond this a set is wider than any preset will use. */
private const val MAX_IMPORT_ITEMS = 300
