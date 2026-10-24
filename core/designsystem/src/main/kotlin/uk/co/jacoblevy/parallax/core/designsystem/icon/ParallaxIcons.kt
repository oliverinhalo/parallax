package uk.co.jacoblevy.parallax.core.designsystem.icon

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Create
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.FavoriteBorder
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowLeft
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Warning
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathBuilder
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.unit.dp

/**
 * Every icon the app uses, named for what it means rather than what it looks like.
 *
 * Two sources, deliberately behind one object. Generic actions come from
 * `material-icons-core`, which is Apache-2.0 and already in the dependency graph; the
 * domain-specific ones -- a mesh, a splat, a floor plan -- do not exist in any icon set and are
 * drawn here as stroked geometry, which matches the geometric grotesk type and the hairline
 * borders used across the UI.
 *
 * Call sites never reference `Icons.Filled.*` directly. That indirection is what makes the
 * Material set replaceable in one file, and it is why the dependency is `implementation` rather
 * than `api` in this module's build script.
 */
object ParallaxIcons {

    // ---- generic actions, from the Material core set -----------------------

    val Add get() = Icons.Filled.Add
    val Back get() = Icons.AutoMirrored.Filled.ArrowBack
    val Check get() = Icons.Filled.Check
    val Clear get() = Icons.Filled.Clear
    val Close get() = Icons.Filled.Close
    val Delete get() = Icons.Filled.Delete
    val Edit get() = Icons.Filled.Create
    val Favourite get() = Icons.Filled.Favorite
    val FavouriteOutline get() = Icons.Filled.FavoriteBorder
    val Info get() = Icons.Filled.Info
    val ChevronDown get() = Icons.Filled.KeyboardArrowDown
    val ChevronLeft get() = Icons.AutoMirrored.Filled.KeyboardArrowLeft
    val ChevronRight get() = Icons.AutoMirrored.Filled.KeyboardArrowRight
    val ChevronUp get() = Icons.Filled.KeyboardArrowUp
    val ListView get() = Icons.AutoMirrored.Filled.List
    val More get() = Icons.Filled.MoreVert
    val Play get() = Icons.Filled.PlayArrow
    val Retry get() = Icons.Filled.Refresh
    val Search get() = Icons.Filled.Search
    val Settings get() = Icons.Filled.Settings
    val Share get() = Icons.Filled.Share
    val Warning get() = Icons.Filled.Warning

    // ---- capture modes -----------------------------------------------------

    /** Object mode: a cube seen in three-quarter view. */
    val ModeObject: ImageVector by lazy {
        lineIcon("ModeObject") {
            moveTo(12f, 2.5f)
            lineTo(20.5f, 7f)
            lineTo(20.5f, 17f)
            lineTo(12f, 21.5f)
            lineTo(3.5f, 17f)
            lineTo(3.5f, 7f)
            close()
            // The three edges that meet at the centre are what read as "solid" rather than
            // "hexagon"; without them this is a flat shape.
            moveTo(3.5f, 7f)
            lineTo(12f, 11.5f)
            lineTo(20.5f, 7f)
            moveTo(12f, 11.5f)
            lineTo(12f, 21.5f)
        }
    }

    /** Room mode: a plan with one internal wall and a doorway gap. */
    val ModeRoom: ImageVector by lazy {
        lineIcon("ModeRoom") {
            moveTo(3.5f, 4f)
            lineTo(20.5f, 4f)
            lineTo(20.5f, 20f)
            lineTo(3.5f, 20f)
            close()
            moveTo(13f, 4f)
            lineTo(13f, 10f)
            moveTo(13f, 14f)
            lineTo(13f, 20f)
        }
    }

    /** Area mode: an open horizon, to distinguish it from an enclosed room. */
    val ModeArea: ImageVector by lazy {
        lineIcon("ModeArea") {
            moveTo(2.5f, 18f)
            lineTo(21.5f, 18f)
            moveTo(5f, 18f)
            lineTo(10f, 9f)
            lineTo(14f, 15f)
            lineTo(16.5f, 11f)
            lineTo(20f, 18f)
            moveTo(2.5f, 6f)
            lineTo(7f, 6f)
        }
    }

    /** Import mode: two stacked images, i.e. a set of existing photos. */
    val ModeImport: ImageVector by lazy {
        lineIcon("ModeImport") {
            moveTo(7.5f, 3.5f)
            lineTo(21f, 3.5f)
            lineTo(21f, 15f)
            moveTo(3f, 8f)
            lineTo(16.5f, 8f)
            lineTo(16.5f, 20.5f)
            lineTo(3f, 20.5f)
            close()
            moveTo(3f, 17f)
            lineTo(7.5f, 12.5f)
            lineTo(11f, 16f)
            lineTo(13f, 14f)
            lineTo(16.5f, 17.5f)
        }
    }

    // ---- capture controls --------------------------------------------------

    /** The capture scan target: four corner brackets. */
    val Scan: ImageVector by lazy {
        lineIcon("Scan") {
            moveTo(3.5f, 9f)
            lineTo(3.5f, 3.5f)
            lineTo(9f, 3.5f)
            moveTo(15f, 3.5f)
            lineTo(20.5f, 3.5f)
            lineTo(20.5f, 9f)
            moveTo(20.5f, 15f)
            lineTo(20.5f, 20.5f)
            lineTo(15f, 20.5f)
            moveTo(9f, 20.5f)
            lineTo(3.5f, 20.5f)
            lineTo(3.5f, 15f)
        }
    }

    val Pause: ImageVector by lazy {
        lineIcon("Pause") {
            moveTo(9f, 4.5f)
            lineTo(9f, 19.5f)
            moveTo(15f, 4.5f)
            lineTo(15f, 19.5f)
        }
    }

    val Stop: ImageVector by lazy {
        lineIcon("Stop") {
            moveTo(5.5f, 5.5f)
            lineTo(18.5f, 5.5f)
            lineTo(18.5f, 18.5f)
            lineTo(5.5f, 18.5f)
            close()
        }
    }

    /** Thermal state, for the capture HUD's warning row. */
    val Thermal: ImageVector by lazy {
        lineIcon("Thermal") {
            moveTo(10f, 13.5f)
            lineTo(10f, 4.5f)
            arcTo(2f, 2f, 0f, false, true, 14f, 4.5f)
            lineTo(14f, 13.5f)
            arcTo(3.2f, 3.2f, 0f, true, true, 10f, 13.5f)
            close()
        }
    }

    val Battery: ImageVector by lazy {
        lineIcon("Battery") {
            moveTo(3.5f, 7.5f)
            lineTo(17f, 7.5f)
            lineTo(17f, 16.5f)
            lineTo(3.5f, 16.5f)
            close()
            moveTo(19.5f, 10.5f)
            lineTo(19.5f, 13.5f)
        }
    }

    // ---- outputs and render modes ------------------------------------------

    /** A mesh: a triangulated patch. */
    val Mesh: ImageVector by lazy {
        lineIcon("Mesh") {
            moveTo(3.5f, 19.5f)
            lineTo(12f, 4.5f)
            lineTo(20.5f, 19.5f)
            close()
            moveTo(7.75f, 12f)
            lineTo(16.25f, 12f)
            moveTo(12f, 4.5f)
            lineTo(12f, 19.5f)
        }
    }

    /** A point cloud: scattered samples rather than a surface. */
    val PointCloud: ImageVector by lazy {
        dotIcon(
            "PointCloud",
            listOf(
                6f to 7f, 12f to 5f, 18f to 8f,
                4.5f to 13f, 10f to 11.5f, 16f to 13f, 20f to 14.5f,
                7f to 18f, 13f to 17.5f, 18.5f to 19.5f,
            ),
            radius = 1.35f,
        )
    }

    /** Gaussian splats: overlapping soft blobs of differing size. */
    val Splat: ImageVector by lazy {
        dotIcon(
            "Splat",
            listOf(9f to 9.5f, 15f to 8f, 12.5f to 15f, 7f to 16f, 18f to 15f),
            radius = 3.4f,
            filled = true,
        )
    }

    /** A measured floor plan: a plan outline with a dimension line beneath it. */
    val FloorPlan: ImageVector by lazy {
        lineIcon("FloorPlan") {
            moveTo(4f, 3.5f)
            lineTo(20f, 3.5f)
            lineTo(20f, 14f)
            lineTo(4f, 14f)
            close()
            moveTo(12.5f, 3.5f)
            lineTo(12.5f, 14f)
            moveTo(4f, 18.5f)
            lineTo(20f, 18.5f)
            moveTo(4f, 17f)
            lineTo(4f, 20f)
            moveTo(20f, 17f)
            lineTo(20f, 20f)
        }
    }

    /** Wireframe render mode. */
    val Wireframe: ImageVector by lazy {
        lineIcon("Wireframe") {
            moveTo(3.5f, 6.5f)
            lineTo(12f, 3f)
            lineTo(20.5f, 6.5f)
            lineTo(20.5f, 17.5f)
            lineTo(12f, 21f)
            lineTo(3.5f, 17.5f)
            close()
            moveTo(3.5f, 6.5f)
            lineTo(20.5f, 17.5f)
            moveTo(20.5f, 6.5f)
            lineTo(3.5f, 17.5f)
            moveTo(12f, 3f)
            lineTo(12f, 21f)
        }
    }

    /** Textured render mode: a surface with a lit highlight. */
    val Textured: ImageVector by lazy {
        lineIcon("Textured") {
            moveTo(12f, 3f)
            arcTo(9f, 9f, 0f, true, true, 11.9f, 3f)
            close()
            moveTo(8f, 8.5f)
            arcTo(2.6f, 2.6f, 0f, true, true, 7.9f, 8.5f)
            close()
        }
    }

    /** Lighting presets. */
    val Lighting: ImageVector by lazy {
        lineIcon("Lighting") {
            moveTo(12f, 7.5f)
            arcTo(4.5f, 4.5f, 0f, true, true, 11.9f, 7.5f)
            close()
            moveTo(12f, 1.5f)
            lineTo(12f, 3.5f)
            moveTo(12f, 20.5f)
            lineTo(12f, 22.5f)
            moveTo(1.5f, 12f)
            lineTo(3.5f, 12f)
            moveTo(20.5f, 12f)
            lineTo(22.5f, 12f)
            moveTo(4.6f, 4.6f)
            lineTo(6f, 6f)
            moveTo(18f, 18f)
            lineTo(19.4f, 19.4f)
            moveTo(19.4f, 4.6f)
            lineTo(18f, 6f)
            moveTo(6f, 18f)
            lineTo(4.6f, 19.4f)
        }
    }

    // ---- viewer tools ------------------------------------------------------

    /** Measurement: a ruler with ticks. */
    val Measure: ImageVector by lazy {
        lineIcon("Measure") {
            moveTo(2.5f, 8.5f)
            lineTo(21.5f, 8.5f)
            lineTo(21.5f, 15.5f)
            lineTo(2.5f, 15.5f)
            close()
            moveTo(7f, 8.5f)
            lineTo(7f, 12f)
            moveTo(11.5f, 8.5f)
            lineTo(11.5f, 12f)
            moveTo(16f, 8.5f)
            lineTo(16f, 12f)
        }
    }

    /** An annotation pin. */
    val Annotation: ImageVector by lazy {
        lineIcon("Annotation") {
            moveTo(12f, 21.5f)
            lineTo(12f, 13f)
            moveTo(12f, 13f)
            arcTo(4.5f, 4.5f, 0f, true, true, 11.9f, 13f)
            close()
        }
    }

    /** Crop box editing. */
    val Crop: ImageVector by lazy {
        lineIcon("Crop") {
            moveTo(6.5f, 2.5f)
            lineTo(6.5f, 17.5f)
            lineTo(21.5f, 17.5f)
            moveTo(2.5f, 6.5f)
            lineTo(17.5f, 6.5f)
            lineTo(17.5f, 21.5f)
        }
    }

    /** Walk mode: a figure mid-stride. */
    val Walk: ImageVector by lazy {
        lineIcon("Walk") {
            moveTo(12f, 2.5f)
            arcTo(1.9f, 1.9f, 0f, true, true, 11.9f, 2.5f)
            close()
            moveTo(12f, 6.5f)
            lineTo(12f, 13.5f)
            moveTo(12f, 13.5f)
            lineTo(8.5f, 21f)
            moveTo(12f, 13.5f)
            lineTo(15.5f, 21f)
            moveTo(12f, 9f)
            lineTo(8f, 11f)
            moveTo(12f, 9f)
            lineTo(16.5f, 10.5f)
        }
    }

    /** Re-level: a horizon with a tilt indicator. */
    val Level: ImageVector by lazy {
        lineIcon("Level") {
            moveTo(2.5f, 15f)
            lineTo(21.5f, 15f)
            moveTo(5f, 9.5f)
            lineTo(19f, 6f)
            moveTo(12f, 15f)
            lineTo(12f, 18.5f)
        }
    }

    val Undo: ImageVector by lazy {
        lineIcon("Undo") {
            moveTo(9f, 5f)
            lineTo(4f, 10f)
            lineTo(9f, 15f)
            moveTo(4f, 10f)
            lineTo(14.5f, 10f)
            arcTo(4.75f, 4.75f, 0f, false, true, 14.5f, 19.5f)
            lineTo(9.5f, 19.5f)
        }
    }

    // ---- library and storage ----------------------------------------------

    val Folder: ImageVector by lazy {
        lineIcon("Folder") {
            moveTo(3f, 19.5f)
            lineTo(3f, 5f)
            lineTo(9.5f, 5f)
            lineTo(11.5f, 8f)
            lineTo(21f, 8f)
            lineTo(21f, 19.5f)
            close()
        }
    }

    val Storage: ImageVector by lazy {
        lineIcon("Storage") {
            moveTo(3.5f, 6.5f)
            lineTo(20.5f, 6.5f)
            lineTo(20.5f, 10.5f)
            lineTo(3.5f, 10.5f)
            close()
            moveTo(3.5f, 13.5f)
            lineTo(20.5f, 13.5f)
            lineTo(20.5f, 17.5f)
            lineTo(3.5f, 17.5f)
            close()
            moveTo(6.5f, 8.5f)
            lineTo(6.6f, 8.5f)
            moveTo(6.5f, 15.5f)
            lineTo(6.6f, 15.5f)
        }
    }

    /** Export, i.e. out of the app. */
    val Export: ImageVector by lazy {
        lineIcon("Export") {
            moveTo(12f, 15.5f)
            lineTo(12f, 3.5f)
            moveTo(7.5f, 8f)
            lineTo(12f, 3.5f)
            lineTo(16.5f, 8f)
            moveTo(3.5f, 15f)
            lineTo(3.5f, 20.5f)
            lineTo(20.5f, 20.5f)
            lineTo(20.5f, 15f)
        }
    }

    /** Sort order. */
    val Sort: ImageVector by lazy {
        lineIcon("Sort") {
            moveTo(4f, 6.5f)
            lineTo(20f, 6.5f)
            moveTo(4f, 12f)
            lineTo(15f, 12f)
            moveTo(4f, 17.5f)
            lineTo(10f, 17.5f)
        }
    }

    /** Filter. */
    val Filter: ImageVector by lazy {
        lineIcon("Filter") {
            moveTo(3f, 4.5f)
            lineTo(21f, 4.5f)
            lineTo(14f, 12.5f)
            lineTo(14f, 20f)
            lineTo(10f, 17.5f)
            lineTo(10f, 12.5f)
            close()
        }
    }

    /** Grid layout. */
    val Grid: ImageVector by lazy {
        lineIcon("Grid") {
            moveTo(3.5f, 3.5f)
            lineTo(10.5f, 3.5f)
            lineTo(10.5f, 10.5f)
            lineTo(3.5f, 10.5f)
            close()
            moveTo(13.5f, 3.5f)
            lineTo(20.5f, 3.5f)
            lineTo(20.5f, 10.5f)
            lineTo(13.5f, 10.5f)
            close()
            moveTo(3.5f, 13.5f)
            lineTo(10.5f, 13.5f)
            lineTo(10.5f, 20.5f)
            lineTo(3.5f, 20.5f)
            close()
            moveTo(13.5f, 13.5f)
            lineTo(20.5f, 13.5f)
            lineTo(20.5f, 20.5f)
            lineTo(13.5f, 20.5f)
            close()
        }
    }

    /** AR placement. */
    val ArPlace: ImageVector by lazy {
        lineIcon("ArPlace") {
            moveTo(3.5f, 8f)
            lineTo(3.5f, 3.5f)
            lineTo(8f, 3.5f)
            moveTo(16f, 3.5f)
            lineTo(20.5f, 3.5f)
            lineTo(20.5f, 8f)
            moveTo(20.5f, 16f)
            lineTo(20.5f, 20.5f)
            lineTo(16f, 20.5f)
            moveTo(8f, 20.5f)
            lineTo(3.5f, 20.5f)
            lineTo(3.5f, 16f)
            moveTo(12f, 8f)
            lineTo(16f, 10.5f)
            lineTo(12f, 13f)
            lineTo(8f, 10.5f)
            close()
            moveTo(12f, 13f)
            lineTo(12f, 16.5f)
        }
    }

    /** The processing queue. */
    val Queue: ImageVector by lazy {
        lineIcon("Queue") {
            moveTo(9f, 5.5f)
            lineTo(21f, 5.5f)
            moveTo(9f, 12f)
            lineTo(21f, 12f)
            moveTo(9f, 18.5f)
            lineTo(21f, 18.5f)
            moveTo(3.5f, 5.5f)
            lineTo(5.5f, 5.5f)
            moveTo(3.5f, 12f)
            lineTo(5.5f, 12f)
            moveTo(3.5f, 18.5f)
            lineTo(5.5f, 18.5f)
        }
    }
}

/**
 * A stroked 24dp icon.
 *
 * Stroke rather than fill, with round caps and joins: it matches the hairline borders and the
 * geometric type, and it keeps every glyph legible at the 20dp the UI actually draws them at.
 * The colour is a placeholder -- `Icon` tints the whole vector, strokes included.
 */
private fun lineIcon(name: String, build: PathBuilder.() -> Unit): ImageVector =
    ImageVector.Builder(
        name = name,
        defaultWidth = ICON_SIZE.dp,
        defaultHeight = ICON_SIZE.dp,
        viewportWidth = ICON_SIZE,
        viewportHeight = ICON_SIZE,
    ).apply {
        path(
            stroke = SolidColor(Color.Black),
            strokeLineWidth = STROKE_WIDTH,
            strokeLineCap = StrokeCap.Round,
            strokeLineJoin = StrokeJoin.Round,
            pathBuilder = build,
        )
    }.build()

/** An icon made of circles, for the point-cloud and splat glyphs. */
private fun dotIcon(
    name: String,
    centres: List<Pair<Float, Float>>,
    radius: Float,
    filled: Boolean = false,
): ImageVector =
    ImageVector.Builder(
        name = name,
        defaultWidth = ICON_SIZE.dp,
        defaultHeight = ICON_SIZE.dp,
        viewportWidth = ICON_SIZE,
        viewportHeight = ICON_SIZE,
    ).apply {
        for ((x, y) in centres) {
            path(
                fill = if (filled) SolidColor(Color.Black) else null,
                fillAlpha = if (filled) SPLAT_ALPHA else 1f,
                stroke = if (filled) null else SolidColor(Color.Black),
                strokeLineWidth = STROKE_WIDTH,
            ) {
                val diameter = radius * 2f
                moveTo(x, y - radius)
                // Two half-arcs rather than one full turn: a single arc of exactly 360 degrees
                // is degenerate, because its start and end points coincide.
                arcToRelative(radius, radius, 0f, true, true, 0f, diameter)
                arcToRelative(radius, radius, 0f, true, true, 0f, -diameter)
                close()
            }
        }
    }.build()

private const val ICON_SIZE = 24f
private const val STROKE_WIDTH = 1.7f

/** Splats are translucent by nature, and the overlap is the point of the glyph. */
private const val SPLAT_ALPHA = 0.55f
