package uk.co.jacoblevy.parallax.feature.export

import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.core.content.FileProvider
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import uk.co.jacoblevy.parallax.core.designsystem.component.EmptyState
import uk.co.jacoblevy.parallax.core.designsystem.component.LoadingState
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxBadge
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxCard
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxChip
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxIconButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxProgressBar
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxSection
import uk.co.jacoblevy.parallax.core.designsystem.component.BadgeTone
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.ExportFormat
import uk.co.jacoblevy.parallax.core.model.LengthUnit

/** Entry point for the navigation graph. */
@Composable
fun ExportRoute(
    onBack: () -> Unit,
    viewModel: ExportViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val context = LocalContext.current

    val saveLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream"),
    ) { uri -> viewModel.writeTo(uri) }

    LaunchedEffect(viewModel) {
        viewModel.events.collect { event ->
            when (event) {
                is ExportEvent.Share -> {
                    // The FileProvider is declared over one staging directory,
                    // so a share grant cannot reach the rest of the project.
                    val uri = FileProvider.getUriForFile(
                        context,
                        "${context.packageName}.fileprovider",
                        event.file,
                    )
                    val intent = Intent(Intent.ACTION_SEND).apply {
                        type = event.mimeType
                        putExtra(Intent.EXTRA_STREAM, uri)
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    context.startActivity(Intent.createChooser(intent, "Share scan"))
                }
                is ExportEvent.ChooseDestination -> saveLauncher.launch(event.suggestedName)
            }
        }
    }

    ExportScreen(
        state = state,
        onBack = onBack,
        onExport = viewModel::export,
        onUnit = viewModel::setUnit,
        onZUp = viewModel::setZUp,
        onDarkFloorPlan = viewModel::setDarkFloorPlan,
        onDestination = viewModel::setDestination,
        onIncludeRawFrames = viewModel::setIncludeRawFrames,
        onDismissMessage = viewModel::dismissMessage,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ExportScreen(
    state: ExportUiState,
    onBack: () -> Unit,
    onExport: (ExportFormat) -> Unit,
    onUnit: (LengthUnit) -> Unit,
    onZUp: (Boolean) -> Unit,
    onDarkFloorPlan: (Boolean) -> Unit,
    onDestination: (ExportDestination) -> Unit,
    onIncludeRawFrames: (Boolean) -> Unit,
    onDismissMessage: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Scaffold(
        modifier = modifier,
        topBar = {
            TopAppBar(
                title = { Text("Export") },
                navigationIcon = {
                    ParallaxIconButton(
                        icon = ParallaxIcons.Back,
                        contentDescription = "Back",
                        onClick = onBack,
                    )
                },
            )
        },
    ) { padding ->
        when {
            state.loading -> LoadingState(Modifier.fillMaxSize().padding(padding))
            state.project == null -> EmptyState(
                modifier = Modifier.fillMaxSize().padding(padding),
                icon = ParallaxIcons.Warning,
                title = "Nothing to export",
                message = state.error ?: "This scan is no longer here.",
            )
            else -> LazyColumn(
                Modifier.fillMaxSize().padding(padding).testTag("exportList"),
                contentPadding = PaddingValues(
                    horizontal = Spacing.screenHorizontal,
                    vertical = Spacing.lg,
                ),
                verticalArrangement = Arrangement.spacedBy(Spacing.md),
            ) {
                item {
                    (state.error ?: state.lastResult)?.let { message ->
                        ParallaxCard(onClick = onDismissMessage) {
                            Row(
                                Modifier.fillMaxWidth().padding(Spacing.md),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                ParallaxBadge(
                                    text = if (state.error != null) "Problem" else "Done",
                                    tone = if (state.error != null) {
                                        BadgeTone.Warning
                                    } else {
                                        BadgeTone.Success
                                    },
                                )
                                Spacer(Modifier.width(Spacing.sm))
                                Text(message, style = MaterialTheme.typography.bodyMedium)
                            }
                        }
                    }
                }

                item { DestinationPicker(state, onDestination) }
                item { OptionsPanel(state, onUnit, onZUp, onDarkFloorPlan, onIncludeRawFrames) }

                item {
                    ParallaxSection(title = "Formats") {
                        Text(
                            "Everything is written on this device. Nothing is uploaded.",
                            style = MaterialTheme.typography.bodySmall,
                            color = ParallaxTheme.colors.textSecondary,
                        )
                    }
                }
                items(state.formats, key = { it.name }) { format ->
                    FormatRow(
                        format = format,
                        busy = state.busyWith == format,
                        enabled = state.busyWith == null,
                        onClick = { onExport(format) },
                    )
                }

                if (state.unavailable.isNotEmpty()) {
                    item {
                        ParallaxSection(title = "Not available for this scan") {
                            Text(
                                "These need something this scan does not have — usually a " +
                                    "texture, a point cloud, a splat field or a floor plan.",
                                style = MaterialTheme.typography.bodySmall,
                                color = ParallaxTheme.colors.textSecondary,
                            )
                        }
                    }
                    items(state.unavailable, key = { "off-${it.name}" }) { format ->
                        FormatRow(format = format, busy = false, enabled = false, onClick = {})
                    }
                }
            }
        }
    }
}

@Composable
private fun DestinationPicker(
    state: ExportUiState,
    onDestination: (ExportDestination) -> Unit,
) {
    ParallaxSection(title = "Where it goes") {
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
            verticalArrangement = Arrangement.spacedBy(Spacing.sm),
        ) {
            for (destination in ExportDestination.entries) {
                ParallaxChip(
                    text = destination.label,
                    selected = destination == state.destination,
                    onClick = { onDestination(destination) },
                )
            }
        }
    }
}

@Composable
private fun OptionsPanel(
    state: ExportUiState,
    onUnit: (LengthUnit) -> Unit,
    onZUp: (Boolean) -> Unit,
    onDarkFloorPlan: (Boolean) -> Unit,
    onIncludeRawFrames: (Boolean) -> Unit,
) {
    ParallaxSection(title = "Options") {
        Column(verticalArrangement = Arrangement.spacedBy(Spacing.md)) {
            Text(
                "Units",
                style = MaterialTheme.typography.labelLarge,
                color = ParallaxTheme.colors.textSecondary,
            )
            // Five unit names do not fit one line on a phone, and at a large
            // font scale they do not fit two. Wrapping is the only layout that
            // keeps every option reachable.
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
                verticalArrangement = Arrangement.spacedBy(Spacing.sm),
            ) {
                for (unit in LengthUnit.entries) {
                    ParallaxChip(
                        text = unit.label,
                        selected = unit == state.settings.unit,
                        onClick = { onUnit(unit) },
                    )
                }
            }
            SwitchRow(
                label = "Z up",
                detail = "For CAD and 3D printing tools that expect Z as the vertical axis.",
                checked = state.settings.zUp,
                onCheckedChange = onZUp,
            )
            SwitchRow(
                label = "Light floor plans",
                detail = "Draw floor plans on white, for printing.",
                checked = !state.settings.darkFloorPlan,
                onCheckedChange = { onDarkFloorPlan(!it) },
            )
            SwitchRow(
                label = "Include camera frames in bundles",
                detail = "Much larger, but lets the scan be reprocessed later.",
                checked = state.includeRawFramesInBundle,
                onCheckedChange = onIncludeRawFrames,
            )
        }
    }
}

@Composable
private fun SwitchRow(
    label: String,
    detail: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .semantics { contentDescription = "$label. $detail" },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            Text(
                detail,
                style = MaterialTheme.typography.bodySmall,
                color = ParallaxTheme.colors.textSecondary,
            )
        }
        Spacer(Modifier.width(Spacing.md))
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

@Composable
private fun FormatRow(
    format: ExportFormat,
    busy: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    ParallaxCard(onClick = if (enabled) onClick else null) {
        Column(Modifier.fillMaxWidth().padding(Spacing.md)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    format.label,
                    style = MaterialTheme.typography.titleSmall,
                    color = if (enabled) {
                        ParallaxTheme.colors.textPrimary
                    } else {
                        ParallaxTheme.colors.textDisabled
                    },
                    modifier = Modifier.weight(1f),
                )
                Text(
                    ".${format.extension}",
                    style = MaterialTheme.typography.labelSmall,
                    color = ParallaxTheme.colors.textSecondary,
                )
            }
            Text(
                format.description,
                style = MaterialTheme.typography.bodySmall,
                color = ParallaxTheme.colors.textSecondary,
            )
            if (busy) {
                Spacer(Modifier.width(Spacing.sm))
                ParallaxProgressBar(
                    progress = 0f,
                    indeterminate = true,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }
}
