package uk.co.jacoblevy.parallax.feature.viewer.render

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import uk.co.jacoblevy.parallax.core.common.di.IoDispatcher
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.RenderMode
import uk.co.jacoblevy.parallax.engine.nativebridge.ParallaxEngine
import uk.co.jacoblevy.parallax.feature.viewer.pick.TrianglePicker
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

/** A model ready to be shown: the file to load, and what can be tapped on. */
data class LoadedModel(
    val glb: File,
    val picker: TrianglePicker?,
)

/**
 * Gets a render mode onto the screen.
 *
 * Shaded and textured modes are the pipeline's own `model.glb`. Points and
 * splats are converted to triangle geometry by the native engine the first
 * time they are asked for and then cached beside the outputs, so switching
 * back and forth is instant and a scan that is never viewed as a splat never
 * pays for one. The cache is keyed on the source file's modification time, so
 * a reprocessed scan rebuilds it rather than showing the old one.
 */
@Singleton
class ModelLoader @Inject constructor(
    private val storage: ProjectStorage,
    @IoDispatcher private val io: CoroutineDispatcher,
) {
    private val json = Json { ignoreUnknownKeys = true }

    suspend fun load(projectId: String, mode: RenderMode): Result<LoadedModel> =
        withContext(io) {
            runCatching {
                when (mode) {
                    RenderMode.Shaded, RenderMode.Textured, RenderMode.Wireframe ->
                        LoadedModel(requireMesh(projectId), pickerFor(projectId))
                    RenderMode.PointCloud -> LoadedModel(
                        preview(projectId, ProjectStorage.CLOUD_PLY, POINTS_PREVIEW, "points"),
                        pickerFor(projectId),
                    )
                    RenderMode.Splat -> LoadedModel(
                        preview(projectId, ProjectStorage.SPLAT_PLY, SPLAT_PREVIEW, "splat"),
                        pickerFor(projectId),
                    )
                }
            }
        }

    private fun requireMesh(projectId: String): File {
        val glb = storage.outputFile(projectId, ProjectStorage.MODEL_GLB)
        require(glb.isFile) { "this scan has no model to show" }
        return glb
    }

    /**
     * The tappable surface.
     *
     * Always the mesh, whatever is being drawn: a measurement taken while
     * looking at the point cloud should land on the same surface it would have
     * landed on in shaded mode, and the mesh is the only representation with
     * an unambiguous surface to intersect.
     */
    private fun pickerFor(projectId: String): TrianglePicker? = runCatching {
        val handle = ParallaxEngine.createPipeline(
            """{"projectDir":"${storage.projectDir(projectId).absolutePath}"}""",
        )
        if (handle == 0L) return null
        try {
            val vertices = ParallaxEngine.loadMeshVertices(handle) ?: return null
            val indices = ParallaxEngine.loadMeshIndices(handle) ?: return null
            if (vertices.isEmpty() || indices.isEmpty()) return null
            TrianglePicker(vertices, indices)
        } finally {
            ParallaxEngine.destroyPipeline(handle)
        }
    }.getOrNull()

    private fun preview(
        projectId: String,
        sourceName: String,
        previewName: String,
        kind: String,
    ): File {
        val source = storage.outputFile(projectId, sourceName)
        require(source.isFile) { "this scan has no $kind data" }
        val target = storage.outputFile(projectId, previewName)
        if (target.isFile && target.lastModified() >= source.lastModified()) return target

        val result = json.decodeFromString<PreviewResult>(
            ParallaxEngine.buildPreview(source.absolutePath, target.absolutePath, kind),
        )
        require(result.success) { result.message.ifEmpty { "the preview could not be built" } }
        return target
    }

    private companion object {
        const val POINTS_PREVIEW = "preview_points.glb"
        const val SPLAT_PREVIEW = "preview_splat.glb"
    }
}

@Serializable
internal data class PreviewResult(
    val success: Boolean = false,
    val message: String = "",
    val vertexCount: Long = 0,
    val triangleCount: Long = 0,
)
