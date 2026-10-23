package uk.co.jacoblevy.parallax.feature.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxCard
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxIconButton
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing

/**
 * The open-source licences screen.
 *
 * The list is in code rather than generated from the Gradle graph at build time. That is a
 * deliberate trade-off: a generated list is complete but names every transitive artifact, which
 * is forty-odd lines of `androidx.collection` that tell the reader nothing. This list names the
 * components a person would want to check, including the model weights, which no dependency
 * scanner would ever see. `docs/LICENSES.md` carries the exhaustive version.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LicencesRoute(onBack: () -> Unit) {
    Scaffold(
        containerColor = ParallaxTheme.colors.surfaceSunken,
        topBar = {
            TopAppBar(
                title = { Text("Open-source licences") },
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
        LazyColumn(
            modifier = Modifier.padding(padding).fillMaxSize(),
            contentPadding = PaddingValues(
                horizontal = Spacing.screenHorizontal,
                vertical = Spacing.md,
            ),
            verticalArrangement = Arrangement.spacedBy(Spacing.sm),
        ) {
            item {
                Text(
                    text = "Parallax uses only permissively licensed components. Nothing here " +
                        "is GPL or AGPL, and no model weights restrict commercial use.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = ParallaxTheme.colors.textSecondary,
                    modifier = Modifier.padding(bottom = Spacing.md),
                )
            }
            items(items = Attributions, key = { it.name }) { attribution ->
                ParallaxCard(Modifier.fillMaxWidth()) {
                    Column(
                        Modifier.semantics(mergeDescendants = true) {
                            contentDescription = "${attribution.name}, ${attribution.licence}. " +
                                attribution.purpose
                        },
                    ) {
                        Text(
                            text = attribution.name,
                            style = MaterialTheme.typography.titleSmall,
                            color = ParallaxTheme.colors.textPrimary,
                        )
                        Text(
                            text = attribution.licence,
                            style = MaterialTheme.typography.labelMedium,
                            color = ParallaxTheme.colors.accent,
                        )
                        Text(
                            text = attribution.purpose,
                            style = MaterialTheme.typography.bodySmall,
                            color = ParallaxTheme.colors.textTertiary,
                        )
                    }
                }
            }
        }
    }
}

private val Attributions = listOf(
    Attribution(
        name = "Eigen",
        licence = "MPL-2.0 (built with EIGEN_MPL2_ONLY)",
        purpose = "Linear algebra throughout the reconstruction engine.",
    ),
    Attribution(
        name = "zlib",
        licence = "zlib licence",
        purpose = "Deflate streams inside the PNG and ZIP writers.",
    ),
    Attribution(
        name = "GoogleTest",
        licence = "BSD-3-Clause",
        purpose = "The native engine's test suite. Not shipped in the app.",
    ),
    Attribution(
        name = "ARCore",
        licence = "Apache-2.0",
        purpose = "Camera poses, plane detection and depth during capture.",
    ),
    Attribution(
        name = "Filament",
        licence = "Apache-2.0",
        purpose = "Physically based rendering of textured meshes in the viewer.",
    ),
    Attribution(
        name = "AndroidX, Jetpack Compose and Material 3",
        licence = "Apache-2.0",
        purpose = "UI, navigation, persistence and background work.",
    ),
    Attribution(
        name = "Dagger Hilt",
        licence = "Apache-2.0",
        purpose = "Dependency injection.",
    ),
    Attribution(
        name = "Kotlin, kotlinx.coroutines and kotlinx.serialization",
        licence = "Apache-2.0",
        purpose = "Language runtime, concurrency and the on-disk formats.",
    ),
    Attribution(
        name = "LiteRT",
        licence = "Apache-2.0",
        purpose = "Runs the depth and segmentation models on the device.",
    ),
    Attribution(
        name = "Depth Anything V2 Small",
        licence = "Apache-2.0",
        purpose = "Monocular depth where the device has no depth sensor. Only the Small " +
            "variant is used: the Base and Large weights are non-commercial.",
    ),
    Attribution(
        name = "Inter",
        licence = "SIL Open Font License 1.1",
        purpose = "The interface typeface.",
    ),
    Attribution(
        name = "JetBrains Mono",
        licence = "SIL Open Font License 1.1",
        purpose = "Measurements and numeric readouts.",
    ),
    Attribution(
        name = "Material Symbols",
        licence = "Apache-2.0",
        purpose = "The generic action icons. Parallax's own glyphs are drawn in the app.",
    ),
)
