package uk.co.jacoblevy.parallax.feature.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Snackbar
import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import uk.co.jacoblevy.parallax.core.common.util.formatBytes
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxCard
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxDivider
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxIconButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxOutlinedButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxSection
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxTextButton
import uk.co.jacoblevy.parallax.core.designsystem.component.StatReadout
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.LengthUnit
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ThemeMode

@Composable
fun SettingsRoute(
    onBack: () -> Unit,
    onOpenLicences: () -> Unit,
    viewModel: SettingsViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    SettingsScreen(
        state = state,
        onEvent = viewModel::onEvent,
        onBack = onBack,
        onOpenLicences = onOpenLicences,
        onRefresh = viewModel::refresh,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun SettingsScreen(
    state: SettingsUiState,
    onEvent: (SettingsEvent) -> Unit,
    onBack: () -> Unit,
    onOpenLicences: () -> Unit,
    onRefresh: () -> Unit,
) {
    val snackbar = remember { SnackbarHostState() }
    LaunchedEffect(state.notice) {
        val notice = state.notice ?: return@LaunchedEffect
        snackbar.showSnackbar(notice, duration = SnackbarDuration.Short)
        onEvent(SettingsEvent.DismissNotice)
    }

    Scaffold(
        containerColor = ParallaxTheme.colors.surfaceSunken,
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    ParallaxIconButton(
                        icon = ParallaxIcons.Back,
                        contentDescription = "Back",
                        onClick = onBack,
                    )
                },
                actions = {
                    ParallaxIconButton(
                        icon = ParallaxIcons.Retry,
                        contentDescription = "Recalculate storage",
                        onClick = onRefresh,
                    )
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = ParallaxTheme.colors.surfaceSunken,
                    titleContentColor = ParallaxTheme.colors.textPrimary,
                ),
            )
        },
        snackbarHost = {
            SnackbarHost(snackbar) { data ->
                Snackbar(
                    snackbarData = data,
                    containerColor = ParallaxTheme.colors.surfaceOverlay,
                    contentColor = ParallaxTheme.colors.textPrimary,
                )
            }
        },
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = Spacing.screenHorizontal),
            verticalArrangement = Arrangement.spacedBy(Spacing.xl),
        ) {
            Spacer(Modifier.height(Spacing.sm))
            StorageSection(state, onEvent)
            CaptureSection(state, onEvent)
            ProcessingSection(state, onEvent)
            AppearanceSection(state, onEvent)
            DeviceSection(state)
            AboutSection(state, onOpenLicences, onEvent)
            Spacer(Modifier.height(Spacing.xxxl))
        }
    }

    SettingsDialogs(state, onEvent)
}

@Composable
private fun StorageSection(state: SettingsUiState, onEvent: (SettingsEvent) -> Unit) {
    ParallaxSection(
        title = "Storage",
        subtitle = "${state.projectCount} scans on this device. Nothing is stored anywhere else.",
    ) {
        ParallaxCard {
            Column {
                StorageBar(state.storage)
                Spacer(Modifier.height(Spacing.md))
                StorageLegend(state.storage)
                ParallaxDivider(Modifier.padding(vertical = Spacing.md))
                Box(Modifier.fillMaxWidth()) {
                    StatReadout(
                        label = "Free on device",
                        value = formatBytes(state.storage.deviceFreeBytes),
                    )
                }
                if (state.storage.reclaimableBytes > 0) {
                    Spacer(Modifier.height(Spacing.md))
                    Text(
                        text = "Captured frames and checkpoints from finished scans can be " +
                            "deleted. The models, textures and measurements are kept.",
                        style = MaterialTheme.typography.bodySmall,
                        color = ParallaxTheme.colors.textSecondary,
                    )
                    Spacer(Modifier.height(Spacing.sm))
                    ParallaxOutlinedButton(
                        text = "Free ${formatBytes(state.storage.reclaimableBytes)}",
                        onClick = { onEvent(SettingsEvent.RequestPurgeIntermediates) },
                        enabled = !state.busy && state.reclaimableProjects > 0,
                        icon = ParallaxIcons.Storage,
                    )
                }
            }
        }
    }
}

@Composable
private fun CaptureSection(state: SettingsUiState, onEvent: (SettingsEvent) -> Unit) {
    ParallaxSection(title = "Capture") {
        Column {
            ChoiceRow(
                title = "Default mode",
                options = state.capabilities.availableCaptureModes,
                selected = state.settings.defaultCaptureMode,
                label = ::modeLabel,
                onSelect = { onEvent(SettingsEvent.SetDefaultMode(it)) },
            )
            ChoiceRow(
                title = "Default quality",
                subtitle = "Higher quality takes longer and uses more space.",
                options = QualityPreset.entries,
                selected = state.settings.defaultQuality,
                label = { it.label },
                // A preset the device cannot run is shown but not selectable, so the ceiling is
                // visible rather than the option simply being missing.
                enabledFor = { it.ordinal <= state.capabilities.maxQualityPreset.ordinal },
                onSelect = { onEvent(SettingsEvent.SetDefaultQuality(it)) },
            )
            SwitchRow(
                title = "Haptics",
                subtitle = "Vibrate for guidance and the shutter",
                checked = state.settings.haptics,
                onCheckedChange = { onEvent(SettingsEvent.SetHaptics(it)) },
            )
            SwitchRow(
                title = "Spoken guidance",
                subtitle = "Read capture prompts aloud",
                checked = state.settings.guidanceVoicePrompts,
                onCheckedChange = { onEvent(SettingsEvent.SetVoicePrompts(it)) },
            )
        }
    }
}

@Composable
private fun ProcessingSection(state: SettingsUiState, onEvent: (SettingsEvent) -> Unit) {
    ParallaxSection(title = "Processing") {
        Column {
            SwitchRow(
                title = "Keep captured frames",
                subtitle = "Needed to reprocess a scan at a different quality. Uses a lot of " +
                    "space.",
                checked = state.settings.keepRawFramesAfterProcessing,
                onCheckedChange = { onEvent(SettingsEvent.SetKeepRawFrames(it)) },
            )
            SwitchRow(
                title = "Only process while charging",
                subtitle = "Reconstruction is demanding and warms the phone",
                checked = state.settings.processOnlyWhileCharging,
                onCheckedChange = { onEvent(SettingsEvent.SetChargingOnly(it)) },
            )
            SwitchRow(
                title = "Refine splats in the background",
                subtitle = if (state.capabilities.canTrainSplats) {
                    "Keeps improving a finished scan's splats while charging"
                } else {
                    "This device cannot train splats"
                },
                checked = state.settings.trainSplatsInBackground,
                enabled = state.capabilities.canTrainSplats,
                onCheckedChange = { onEvent(SettingsEvent.SetBackgroundSplats(it)) },
            )
        }
    }
}

@Composable
private fun AppearanceSection(state: SettingsUiState, onEvent: (SettingsEvent) -> Unit) {
    ParallaxSection(title = "Appearance") {
        Column {
            ChoiceRow(
                title = "Theme",
                options = ThemeMode.entries,
                selected = state.settings.theme,
                label = { it.label },
                onSelect = { onEvent(SettingsEvent.SetTheme(it)) },
            )
            SwitchRow(
                title = "Use wallpaper colours",
                subtitle = "Material You instead of the Parallax palette",
                checked = state.settings.useDynamicColor,
                onCheckedChange = { onEvent(SettingsEvent.SetDynamicColor(it)) },
            )
            ChoiceRow(
                title = "Units",
                options = LengthUnit.entries,
                selected = state.settings.units,
                label = { it.label },
                onSelect = { onEvent(SettingsEvent.SetUnits(it)) },
            )
            SwitchRow(
                title = "Show technical stats",
                subtitle = "Reprojection error, voxel size, frame scores",
                checked = state.settings.showAdvancedStats,
                onCheckedChange = { onEvent(SettingsEvent.SetAdvancedStats(it)) },
            )
        }
    }
}

@Composable
private fun DeviceSection(state: SettingsUiState) {
    val limitations = state.capabilities.limitations
    ParallaxSection(
        title = "This device",
        subtitle = if (limitations.isEmpty()) {
            "Everything Parallax can do is available here."
        } else {
            "Some features are limited on this device."
        },
    ) {
        ParallaxCard {
            Column {
                limitations.forEach { limitation ->
                    LimitationRow(
                        title = limitation.title,
                        detail = limitation.detail,
                        affects = limitation.affects,
                    )
                }
                if (limitations.isNotEmpty()) {
                    ParallaxDivider(Modifier.padding(vertical = Spacing.sm))
                }
                Text(
                    text = deviceSummary(state),
                    style = MaterialTheme.typography.bodySmall,
                    color = ParallaxTheme.colors.textTertiary,
                )
            }
        }
    }
}

@Composable
private fun AboutSection(
    state: SettingsUiState,
    onOpenLicences: () -> Unit,
    onEvent: (SettingsEvent) -> Unit,
) {
    ParallaxSection(title = "About") {
        Column {
            Text(
                text = "Parallax works entirely on this device. It makes no network requests " +
                    "for scanning, processing or storage, and collects no analytics.",
                style = MaterialTheme.typography.bodyMedium,
                color = ParallaxTheme.colors.textSecondary,
            )
            Spacer(Modifier.height(Spacing.sm))
            Text(
                text = state.engineUnavailableReason?.let { "Engine unavailable: $it" }
                    ?: "Reconstruction engine ${state.engineVersion}",
                style = MaterialTheme.typography.bodySmall,
                color = if (state.engineUnavailableReason != null) {
                    ParallaxTheme.colors.danger
                } else {
                    ParallaxTheme.colors.textTertiary
                },
            )
            Spacer(Modifier.height(Spacing.md))
            ParallaxTextButton(text = "Open-source licences", onClick = onOpenLicences)
            ParallaxTextButton(
                text = "Reset settings",
                onClick = { onEvent(SettingsEvent.RequestReset) },
                destructive = true,
            )
        }
    }
}

@Composable
private fun SettingsDialogs(state: SettingsUiState, onEvent: (SettingsEvent) -> Unit) {
    if (state.confirmingPurge) {
        AlertDialog(
            onDismissRequest = { onEvent(SettingsEvent.CancelPurgeIntermediates) },
            title = { Text("Delete captured frames?") },
            text = {
                Text(
                    "This frees ${formatBytes(state.storage.reclaimableBytes)} across " +
                        "${state.reclaimableProjects} finished scans. The models and " +
                        "measurements are kept, but those scans can no longer be reprocessed " +
                        "at a different quality.",
                )
            },
            confirmButton = {
                ParallaxTextButton(
                    text = "Delete frames",
                    onClick = { onEvent(SettingsEvent.ConfirmPurgeIntermediates) },
                    destructive = true,
                )
            },
            dismissButton = {
                ParallaxTextButton(
                    text = "Keep",
                    onClick = { onEvent(SettingsEvent.CancelPurgeIntermediates) },
                )
            },
            containerColor = ParallaxTheme.colors.surfaceRaised,
        )
    }

    if (state.confirmingReset) {
        AlertDialog(
            onDismissRequest = { onEvent(SettingsEvent.CancelReset) },
            title = { Text("Reset settings?") },
            text = { Text("Your scans are not affected. Only these preferences are restored.") },
            confirmButton = {
                ParallaxTextButton(
                    text = "Reset",
                    onClick = { onEvent(SettingsEvent.ConfirmReset) },
                    destructive = true,
                )
            },
            dismissButton = {
                ParallaxTextButton(text = "Cancel", onClick = { onEvent(SettingsEvent.CancelReset) })
            },
            containerColor = ParallaxTheme.colors.surfaceRaised,
        )
    }
}

private fun modeLabel(mode: CaptureMode) = when (mode) {
    CaptureMode.Object -> "Object"
    CaptureMode.Room -> "Room"
    CaptureMode.Area -> "Area"
    CaptureMode.Import -> "Import"
}

private fun deviceSummary(state: SettingsUiState): String {
    val capabilities = state.capabilities
    return buildList {
        add("${capabilities.cpuCoreCount} cores")
        if (capabilities.totalRamBytes > 0) add("${formatBytes(capabilities.totalRamBytes)} RAM")
        add(if (capabilities.supportsDepthApi) "depth sensing" else "no depth sensing")
        if (capabilities.hasTimeOfFlightSensor) add("time-of-flight sensor")
        add(if (capabilities.supportsVulkanCompute) "Vulkan compute" else "no Vulkan compute")
        if (capabilities.supportsNnApi) add("NNAPI")
        if (capabilities.supportsGpuDelegate) add("GPU inference")
        add("max texture ${capabilities.maxTextureSize}px")
    }.joinToString(" · ")
}
