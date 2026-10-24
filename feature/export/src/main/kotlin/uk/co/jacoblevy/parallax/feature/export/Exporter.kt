package uk.co.jacoblevy.parallax.feature.export

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import uk.co.jacoblevy.parallax.core.common.di.IoDispatcher
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.ExportFormat
import uk.co.jacoblevy.parallax.core.model.LengthUnit
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.engine.nativebridge.ParallaxEngine
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

/** What the user chose, over and above which format. */
data class ExportSettings(
    val unit: LengthUnit = LengthUnit.Metres,
    val zUp: Boolean = false,
    val includeVertexColours: Boolean = true,
    val maxTextureSize: Int = 4096,
    val targetTriangles: Int = 0,
    val darkFloorPlan: Boolean = true,
)

/** Where a finished export ended up, and what to tell the user about it. */
data class ExportResult(val file: File, val format: ExportFormat, val detail: String)

/**
 * Writes one export.
 *
 * Every format goes through the native engine, which is where all the writers
 * live, so this class is a translator: it decides the file name, picks the
 * right engine call for the format, and turns the engine's JSON answer into
 * either a file or a message a person can act on.
 *
 * Exports are written into the project's own `exports/` directory rather than
 * straight to the destination. A share sheet needs a file that outlives the
 * call, a SAF write needs a source to copy from, and a failed write halfway
 * through should not leave a truncated file in the user's Downloads.
 */
@Singleton
class Exporter @Inject constructor(
    private val storage: ProjectStorage,
    @IoDispatcher private val io: CoroutineDispatcher,
) {
    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

    suspend fun export(
        project: Project,
        format: ExportFormat,
        settings: ExportSettings = ExportSettings(),
    ): Result<ExportResult> = withContext(io) {
        runCatching {
            require(format.isAvailableFor(project.outputs)) {
                "${format.label} needs something this scan does not have."
            }
            val target = destinationFor(project, format)
            target.parentFile?.mkdirs()
            when {
                format.isSplat -> convertSplat(project, format, target)
                format == ExportFormat.ProjectBundle ->
                    error("bundles are written by ProjectTransfer, not here")
                else -> runEngine(project, format, settings, target)
            }
        }
    }

    /** The file name a share sheet or a save dialog should suggest. */
    fun suggestedName(project: Project, format: ExportFormat): String {
        val stem = project.name
            .map { if (it.isLetterOrDigit() || it == '-' || it == '_') it else ' ' }
            .joinToString("")
            .trim()
            .replace(Regex("\\s+"), "-")
            .ifEmpty { "parallax-scan" }
        // glTF is a directory of files, so its "name" is the folder's.
        return if (format == ExportFormat.Gltf) stem else "$stem.${format.extension}"
    }

    /**
     * Where the file is written before it goes anywhere.
     *
     * The multi-file glTF layout gets a directory rather than a file, which is
     * the only case where the two differ; everything else is one file named
     * after the scan.
     */
    private fun destinationFor(project: Project, format: ExportFormat): File =
        File(storage.exportsDir(project.id), suggestedName(project, format))

    private fun convertSplat(project: Project, format: ExportFormat, target: File): ExportResult {
        val source = storage.outputFile(project.id, ProjectStorage.SPLAT_PLY)
        require(source.isFile) { "This scan has no splat field." }
        val kind = when (format) {
            ExportFormat.SplatPly -> "ply"
            ExportFormat.Splat -> "splat"
            else -> "spz"
        }
        val answer = json.decodeFromString<EngineAnswer>(
            ParallaxEngine.convertSplats(source.absolutePath, target.absolutePath, kind),
        )
        require(answer.success) { answer.message.ifEmpty { "The splat file could not be written." } }
        return ExportResult(target, format, "${answer.count} gaussians")
    }

    private fun runEngine(
        project: Project,
        format: ExportFormat,
        settings: ExportSettings,
        target: File,
    ): ExportResult {
        val handle = ParallaxEngine.createPipeline(
            json.encodeToString(PipelineOpen(storage.projectDir(project.id).absolutePath)),
        )
        require(handle != 0L) { "This scan's files could not be opened." }
        try {
            val answer = json.decodeFromString<EngineAnswer>(
                ParallaxEngine.export(
                    handle = handle,
                    format = engineKindFor(format),
                    outputPath = if (format == ExportFormat.Gltf) {
                        target.apply { mkdirs() }.absolutePath
                    } else {
                        target.absolutePath
                    },
                    optionsJson = json.encodeToString(optionsFor(project, settings)),
                ),
            )
            require(answer.success) {
                answer.message.ifEmpty { "${format.label} could not be written." }
            }
            val written = if (format == ExportFormat.Gltf) {
                File(target, "${suggestedName(project, format)}.gltf")
            } else {
                target
            }
            return ExportResult(written, format, answer.message)
        } finally {
            ParallaxEngine.destroyPipeline(handle)
        }
    }

    private fun engineKindFor(format: ExportFormat): String = when (format) {
        ExportFormat.Glb -> "glb"
        ExportFormat.Gltf -> "gltf"
        ExportFormat.Obj -> "obj"
        ExportFormat.Usdz -> "usdz"
        ExportFormat.Stl -> "stl"
        ExportFormat.Ply -> "ply_points"
        ExportFormat.FloorPlanSvg -> "floorplan_svg"
        ExportFormat.FloorPlanPng -> "floorplan_png"
        ExportFormat.FloorPlanPdf -> "floorplan_pdf"
        ExportFormat.HtmlViewer -> "html"
        ExportFormat.SplatPly, ExportFormat.Splat, ExportFormat.Spz, ExportFormat.ProjectBundle ->
            error("${format.label} does not go through the engine's export call")
    }

    private fun optionsFor(project: Project, settings: ExportSettings): ExportOptionsJson {
        val measurements = project.measurements
        return ExportOptionsJson(
            unit = when (settings.unit) {
                LengthUnit.Metres -> "m"
                LengthUnit.Centimetres -> "cm"
                LengthUnit.Millimetres -> "mm"
                LengthUnit.Inches -> "in"
                LengthUnit.Feet -> "ft"
            },
            upAxis = if (settings.zUp) "z" else "y",
            maxTextureSize = settings.maxTextureSize,
            targetTriangles = settings.targetTriangles,
            includeVertexColors = settings.includeVertexColours,
            name = suggestedName(project, ExportFormat.Gltf),
            title = project.name,
            dark = settings.darkFloorPlan,
            imperial = settings.unit.isImperial,
            // Flattened to pairs because the engine's JSON reader has no map
            // type; the bridge reads them two at a time.
            facts = buildList {
                measurements?.let { stats ->
                    if (stats.triangleCount > 0) {
                        add("Triangles")
                        add(stats.triangleCount.toString())
                    }
                    stats.floorAreaSquareMetres?.let {
                        add("Floor area")
                        add(formatMetric(it, "m²", settings.unit.isImperial))
                    }
                    stats.ceilingHeightMetres?.let {
                        add("Ceiling")
                        add(formatMetric(it, "m", settings.unit.isImperial))
                    }
                }
                add("Mode")
                add(project.captureMode.name)
            },
        )
    }

    private fun formatMetric(value: Double, unit: String, imperial: Boolean): String {
        val converted = when {
            !imperial -> value
            unit == "m²" -> value * SQUARE_FEET_PER_SQUARE_METRE
            else -> value / LengthUnit.Feet.metresPerUnit
        }
        val suffix = when {
            !imperial -> unit
            unit == "m²" -> "sq ft"
            else -> "ft"
        }
        val rounded = kotlin.math.round(converted * CENTIMETRE_PRECISION) / CENTIMETRE_PRECISION
        return "$rounded $suffix"
    }

    private companion object {
        const val SQUARE_FEET_PER_SQUARE_METRE = 10.7639104

        /** Two decimal places, which for a room measurement is plenty. */
        const val CENTIMETRE_PRECISION = 100.0
    }
}

@Serializable
private data class PipelineOpen(val projectDir: String)

@Serializable
internal data class ExportOptionsJson(
    val unit: String,
    val upAxis: String,
    val maxTextureSize: Int,
    val targetTriangles: Int,
    val includeVertexColors: Boolean,
    val name: String,
    val title: String,
    val dark: Boolean,
    val imperial: Boolean,
    val facts: List<String>,
)

@Serializable
private data class EngineAnswer(
    val success: Boolean = false,
    val message: String = "",
    val count: Long = 0,
)
