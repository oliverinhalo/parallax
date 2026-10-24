package uk.co.jacoblevy.parallax.core.model

import kotlinx.serialization.Serializable

/** What the viewer is currently drawing. */
@Serializable
enum class RenderMode(val label: String) {
    Shaded("Shaded"),
    Textured("Textured"),
    Wireframe("Wireframe"),
    PointCloud("Points"),
    Splat("Splat"),
    ;

    fun isAvailableFor(outputs: Set<OutputKind>): Boolean = when (this) {
        Shaded, Wireframe -> OutputKind.Mesh in outputs || OutputKind.TexturedMesh in outputs
        Textured -> OutputKind.TexturedMesh in outputs
        PointCloud -> OutputKind.PointCloud in outputs
        Splat -> OutputKind.Splat in outputs
    }
}

@Serializable
enum class LightingPreset(val label: String) {
    Studio("Studio"),
    Neutral("Neutral"),
    Environment("Environment"),
    Flat("Flat"),
}

/** A saved measurement on a model. */
@Serializable
sealed interface Measurement {
    val id: String
    val label: String

    @Serializable
    data class Distance(
        override val id: String,
        override val label: String,
        val fromX: Float, val fromY: Float, val fromZ: Float,
        val toX: Float, val toY: Float, val toZ: Float,
    ) : Measurement {
        val metres: Double
            get() {
                val dx = (toX - fromX).toDouble()
                val dy = (toY - fromY).toDouble()
                val dz = (toZ - fromZ).toDouble()
                return kotlin.math.sqrt(dx * dx + dy * dy + dz * dz)
            }
    }

    /** Vertical extent between two points, ignoring their horizontal separation. */
    @Serializable
    data class Height(
        override val id: String,
        override val label: String,
        val baseY: Float,
        val topY: Float,
    ) : Measurement {
        val metres: Double get() = kotlin.math.abs(topY - baseY).toDouble()
    }

    /** A polygon's area, computed from its vertices via the shoelace formula in-plane. */
    @Serializable
    data class Area(
        override val id: String,
        override val label: String,
        val points: List<Point3>,
    ) : Measurement
}

@Serializable
data class Point3(val x: Float, val y: Float, val z: Float)

/** A pin the user dropped on the model, with a note. */
@Serializable
data class Annotation(
    val id: String,
    val title: String,
    val body: String,
    val position: Point3,
    val normal: Point3? = null,
    val colorArgb: Int = 0xFF3DE0FF.toInt(),
    val createdAtMillis: Long = 0,
)

/** A non-destructive edit, recorded so it can be re-applied or undone. */
@Serializable
sealed interface ModelEdit {
    @Serializable
    data class CropBox(val box: BoundingBox) : ModelEdit

    @Serializable
    data class LassoDelete(val screenPolygon: List<Point2>, val viewMatrix: List<Float>) : ModelEdit

    @Serializable
    data class Relevel(val rotation: List<Float>) : ModelEdit

    /** Rescale so a measured distance equals a known real one. */
    @Serializable
    data class Rescale(val measuredMetres: Double, val actualMetres: Double) : ModelEdit {
        val factor: Double get() = if (measuredMetres > 1e-9) actualMetres / measuredMetres else 1.0
    }

    @Serializable
    data class TextureAdjust(
        val exposureStops: Float = 0f,
        val saturation: Float = 1f,
        val warmth: Float = 0f,
    ) : ModelEdit
}

@Serializable
data class Point2(val x: Float, val y: Float)
