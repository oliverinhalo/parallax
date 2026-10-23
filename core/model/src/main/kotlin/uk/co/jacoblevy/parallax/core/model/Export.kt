package uk.co.jacoblevy.parallax.core.model

import kotlinx.serialization.Serializable

/** Every format a project can be exported to. */
@Serializable
enum class ExportFormat(
    val label: String,
    val extension: String,
    val mimeType: String,
    val requires: OutputKind,
    val description: String,
) {
    Glb(
        label = "GLB",
        extension = "glb",
        mimeType = "model/gltf-binary",
        requires = OutputKind.Mesh,
        description = "Single-file glTF. The best general-purpose choice.",
    ),
    Gltf(
        label = "glTF",
        extension = "gltf",
        mimeType = "model/gltf+json",
        requires = OutputKind.Mesh,
        description = "glTF with separate buffer and texture files.",
    ),
    Obj(
        label = "OBJ",
        extension = "obj",
        mimeType = "model/obj",
        requires = OutputKind.Mesh,
        description = "Widely supported. Writes OBJ, MTL and a PNG texture.",
    ),
    Usdz(
        label = "USDZ",
        extension = "usdz",
        mimeType = "model/vnd.usdz+zip",
        requires = OutputKind.Mesh,
        description = "Opens in Quick Look on Apple devices.",
    ),
    Stl(
        label = "STL",
        extension = "stl",
        mimeType = "model/stl",
        requires = OutputKind.WatertightMesh,
        description = "For 3D printing. Watertight, with a units choice.",
    ),
    Ply(
        label = "PLY",
        extension = "ply",
        mimeType = "application/octet-stream",
        requires = OutputKind.PointCloud,
        description = "Coloured point cloud.",
    ),
    SplatPly(
        label = "Splat PLY",
        extension = "ply",
        mimeType = "application/octet-stream",
        requires = OutputKind.Splat,
        description = "Gaussian splats in the standard 3DGS layout.",
    ),
    Splat(
        label = "SPLAT",
        extension = "splat",
        mimeType = "application/octet-stream",
        requires = OutputKind.Splat,
        description = "Compact splat format for web viewers.",
    ),
    Spz(
        label = "SPZ",
        extension = "spz",
        mimeType = "application/octet-stream",
        requires = OutputKind.Splat,
        description = "Compressed splats, around a tenth the size of PLY.",
    ),
    FloorPlanSvg(
        label = "Floor plan (SVG)",
        extension = "svg",
        mimeType = "image/svg+xml",
        requires = OutputKind.FloorPlan,
        description = "Vector floor plan with dimensions.",
    ),
    FloorPlanPdf(
        label = "Floor plan (PDF)",
        extension = "pdf",
        mimeType = "application/pdf",
        requires = OutputKind.FloorPlan,
        description = "Printable floor plan.",
    ),
    FloorPlanPng(
        label = "Floor plan (PNG)",
        extension = "png",
        mimeType = "image/png",
        requires = OutputKind.FloorPlan,
        description = "Floor plan image.",
    ),
    HtmlViewer(
        label = "Web viewer",
        extension = "html",
        mimeType = "text/html",
        requires = OutputKind.Mesh,
        description = "One self-contained HTML file anyone can open in a browser.",
    ),
    ProjectBundle(
        label = "Parallax bundle",
        extension = "parallax",
        mimeType = "application/zip",
        requires = OutputKind.Mesh,
        description = "Everything, for backup or moving to another device.",
    ),
    ;

    val isFloorPlan: Boolean
        get() = this == FloorPlanSvg || this == FloorPlanPdf || this == FloorPlanPng

    val isSplat: Boolean get() = this == SplatPly || this == Splat || this == Spz

    fun isAvailableFor(outputs: Set<OutputKind>): Boolean = when (requires) {
        // A watertight variant is produced alongside the mesh, so having a mesh is enough.
        OutputKind.WatertightMesh -> OutputKind.WatertightMesh in outputs || OutputKind.Mesh in outputs
        else -> requires in outputs
    }
}

/** The options an export dialog collects. */
@Serializable
data class ExportOptions(
    val format: ExportFormat,
    val textureResolution: Int = 4096,
    val decimationPercent: Int = 100,
    val upAxis: UpAxis = UpAxis.YUp,
    val unit: LengthUnit = LengthUnit.Metres,
    val coordinateSystem: CoordinateSystem = CoordinateSystem.RightHanded,
    val includeVertexColors: Boolean = true,
    val includeNormals: Boolean = true,
    val embedTextures: Boolean = true,
) {
    /** Target triangle count given the source mesh size. */
    fun targetTriangles(sourceTriangles: Long): Int =
        (sourceTriangles * decimationPercent / PERCENT).toInt().coerceAtLeast(MIN_TRIANGLES)

    private companion object {
        const val PERCENT = 100

        /** A tetrahedron: the fewest triangles that still enclose a volume. */
        const val MIN_TRIANGLES = 4
    }
}

@Serializable
enum class UpAxis(val label: String) { YUp("Y up"), ZUp("Z up") }

@Serializable
enum class CoordinateSystem(val label: String) {
    RightHanded("Right-handed"),
    LeftHanded("Left-handed"),
}

@Serializable
enum class LengthUnit(val label: String, val metresPerUnit: Double) {
    Metres("Metres", 1.0),
    Centimetres("Centimetres", 0.01),
    Millimetres("Millimetres", 0.001),
    Inches("Inches", 0.0254),
    Feet("Feet", 0.3048),
    ;

    val isImperial: Boolean get() = this == Inches || this == Feet
}
