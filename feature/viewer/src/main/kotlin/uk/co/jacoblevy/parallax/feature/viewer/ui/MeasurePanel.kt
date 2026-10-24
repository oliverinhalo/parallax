package uk.co.jacoblevy.parallax.feature.viewer.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import uk.co.jacoblevy.parallax.core.designsystem.component.GlassPanel
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxChip
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxIconButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxTextButton
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.feature.viewer.ViewerUiState
import uk.co.jacoblevy.parallax.feature.viewer.ViewerViewModel
import uk.co.jacoblevy.parallax.feature.viewer.measure.MeasureTool
import uk.co.jacoblevy.parallax.feature.viewer.measure.formatArea
import uk.co.jacoblevy.parallax.feature.viewer.measure.formatLength
import uk.co.jacoblevy.parallax.feature.viewer.measure.squareMetres

/**
 * The measuring tool: which kind, how far along, and what has been saved.
 *
 * The live value is shown while the points are being placed rather than only
 * once saved, because the whole reason to place the second point carefully is
 * to watch the number.
 */
@Composable
internal fun MeasurePanel(state: ViewerUiState, viewModel: ViewerViewModel) {
    GlassPanel(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = Spacing.screenHorizontal, vertical = Spacing.sm)
            .testTag("measurePanel"),
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
            Row(
                Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
            ) {
                for (tool in MeasureTool.entries) {
                    ParallaxChip(
                        text = tool.label,
                        selected = tool == state.measureTool,
                        onClick = { viewModel.setMeasureTool(tool) },
                    )
                }
            }

            val draft = state.draft
            val preview = draft?.previewMetres()
            Text(
                text = when {
                    draft == null -> "Tap the model to start measuring."
                    preview != null && draft.tool == MeasureTool.Area ->
                        formatArea(preview, state.imperialUnits)
                    preview != null -> formatLength(preview, state.imperialUnits)
                    draft.points.isEmpty() -> "Tap the first point."
                    else -> "Tap the next point."
                },
                style = MaterialTheme.typography.headlineSmall,
                color = ParallaxTheme.colors.textPrimary,
            )

            Row(horizontalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                ParallaxTextButton(
                    text = "Undo point",
                    onClick = viewModel::undoMeasurePoint,
                    enabled = (draft?.points?.size ?: 0) > 0,
                )
                if (draft?.tool == MeasureTool.Area) {
                    ParallaxTextButton(
                        text = "Finish area",
                        onClick = viewModel::saveDraft,
                        enabled = draft.canFinish,
                    )
                }
            }

            if (state.measurements.isNotEmpty()) {
                Text(
                    "Saved",
                    style = MaterialTheme.typography.labelLarge,
                    color = ParallaxTheme.colors.textSecondary,
                )
                LazyColumn(Modifier.heightIn(max = SAVED_LIST_MAX_HEIGHT.dp)) {
                    items(state.measurements, key = { it.id }) { measurement ->
                        SavedMeasurementRow(
                            measurement = measurement,
                            imperial = state.imperialUnits,
                            onDelete = { viewModel.deleteMeasurement(measurement.id) },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun SavedMeasurementRow(
    measurement: Measurement,
    imperial: Boolean,
    onDelete: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().padding(vertical = Spacing.xs),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                measurement.label,
                style = MaterialTheme.typography.bodyMedium,
                color = ParallaxTheme.colors.textPrimary,
            )
            Text(
                text = when (measurement) {
                    is Measurement.Distance -> formatLength(measurement.metres, imperial)
                    is Measurement.Height -> formatLength(measurement.metres, imperial)
                    is Measurement.Area -> formatArea(measurement.squareMetres, imperial)
                },
                style = MaterialTheme.typography.bodyLarge,
                color = ParallaxTheme.colors.accent,
            )
        }
        ParallaxIconButton(
            icon = ParallaxIcons.Delete,
            contentDescription = "Delete ${measurement.label}",
            onClick = onDelete,
        )
    }
}

/** The note being written for a pin that has just been dropped. */
@Composable
internal fun AnnotationComposer(viewModel: ViewerViewModel) {
    var title by remember { mutableStateOf("") }
    var body by remember { mutableStateOf("") }

    GlassPanel(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = Spacing.screenHorizontal, vertical = Spacing.sm)
            .testTag("annotationComposer"),
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
            Text(
                "Note on this spot",
                style = MaterialTheme.typography.titleSmall,
                color = ParallaxTheme.colors.textPrimary,
            )
            OutlinedTextField(
                value = title,
                onValueChange = { title = it.take(TITLE_LIMIT) },
                label = { Text("Title") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = body,
                onValueChange = { body = it.take(BODY_LIMIT) },
                label = { Text("Detail") },
                minLines = 2,
                modifier = Modifier.fillMaxWidth(),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                ParallaxTextButton(text = "Cancel", onClick = viewModel::cancelAnnotation)
                Spacer(Modifier.weight(1f))
                ParallaxTextButton(
                    text = "Save note",
                    onClick = { viewModel.saveAnnotation(title, body) },
                )
            }
        }
    }
}

private const val SAVED_LIST_MAX_HEIGHT = 180
private const val TITLE_LIMIT = 60
private const val BODY_LIMIT = 500
