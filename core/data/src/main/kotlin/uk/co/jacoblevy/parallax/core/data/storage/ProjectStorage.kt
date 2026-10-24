package uk.co.jacoblevy.parallax.core.data.storage

import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import uk.co.jacoblevy.parallax.core.model.CaptureFrame
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.StorageBreakdown
import java.io.File
import java.io.IOException
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The on-disk layout of a project, and the single place that knows it.
 *
 * ```
 * files/projects/<project id>/
 *   manifest.json        frames, poses and intrinsics; the native pipeline's only input
 *   frames/              captured colour frames, as JPEG
 *   depth/               sensor or estimated depth, as raw 16-bit maps
 *   checkpoints/         per-stage pipeline state, so processing resumes after a kill
 *   outputs/             model.glb, cloud.ply, splat.spz, floorplan.*
 *   exports/             files written for sharing, kept only until the share completes
 *   thumbnail.jpg
 * ```
 *
 * The native pipeline is handed `projectDir` and reads and writes these paths itself, so the
 * names here are a contract with C++ (`Pipeline.cpp`), not an implementation detail. They are
 * deliberately not configurable: a mismatch between the two sides would surface as an empty
 * reconstruction rather than an error.
 */
@Singleton
class ProjectStorage @Inject constructor(
    @ApplicationContext private val context: Context,
) {

    /**
     * `explicitNulls = false` is load-bearing, not tidiness.
     *
     * The native pipeline decides whether a capture has camera poses with `entry.has("pose")`.
     * A frame written as `"pose": null` answers yes, and the pipeline would then read an
     * identity matrix for every frame -- turning an import that should run full
     * structure-from-motion into an AR capture whose cameras are all at the origin, with no
     * error anywhere to explain the empty result.
     */
    private val json = Json {
        prettyPrint = true
        encodeDefaults = true
        explicitNulls = false
        ignoreUnknownKeys = true
    }

    val root: File get() = File(context.filesDir, PROJECTS).apply { mkdirs() }

    fun projectDir(projectId: String): File = File(root, projectId)
    fun framesDir(projectId: String): File = File(projectDir(projectId), FRAMES)
    fun depthDir(projectId: String): File = File(projectDir(projectId), DEPTH)
    fun checkpointsDir(projectId: String): File = File(projectDir(projectId), CHECKPOINTS)
    fun outputsDir(projectId: String): File = File(projectDir(projectId), OUTPUTS)
    fun exportsDir(projectId: String): File = File(projectDir(projectId), EXPORTS)
    fun manifestFile(projectId: String): File = File(projectDir(projectId), MANIFEST)
    fun thumbnailFile(projectId: String): File = File(projectDir(projectId), THUMBNAIL)

    fun outputFile(projectId: String, name: String): File = File(outputsDir(projectId), name)

    /**
     * The one directory the share sheet can read.
     *
     * Separate from `projects/` on purpose. A `FileProvider` path has no wildcards, so rooting
     * it at `projects/` to reach each project's `exports/` would expose every project's frames,
     * checkpoints and outputs to whatever app receives a share URI. Files are copied here for
     * the duration of a share and swept afterwards.
     */
    fun sharedDir(): File = File(context.filesDir, SHARED).apply { mkdirs() }

    fun sharedFile(name: String): File = File(sharedDir(), name)

    /** Delete everything staged for sharing. Called at startup and after a share completes. */
    fun clearShared(): Long {
        val dir = sharedDir()
        val freed = directorySize(dir)
        dir.deleteRecursively()
        dir.mkdirs()
        return freed
    }

    /** Create the directory tree for a new project. Idempotent, so resuming a capture is safe. */
    @Throws(IOException::class)
    fun create(projectId: String): File {
        val dir = projectDir(projectId)
        for (child in listOf(FRAMES, DEPTH, CHECKPOINTS, OUTPUTS, EXPORTS)) {
            val sub = File(dir, child)
            if (!sub.isDirectory && !sub.mkdirs()) {
                throw IOException("could not create ${sub.absolutePath}")
            }
        }
        return dir
    }

    fun exists(projectId: String): Boolean = projectDir(projectId).isDirectory

    /** Project ids with a directory on disk, used to reconcile the database against reality. */
    fun listProjectIds(): List<String> =
        root.listFiles { file -> file.isDirectory }?.map { it.name }?.sorted() ?: emptyList()

    fun delete(projectId: String): Boolean = projectDir(projectId).deleteRecursively()

    fun sizeOf(projectId: String): Long = directorySize(projectDir(projectId))

    /**
     * Delete everything that can be regenerated or that was only needed to build the model.
     *
     * Called after a successful run when the user has not asked to keep raw frames. This is
     * where most of the space goes: a 160-frame capture is around 400 MB of JPEG and depth,
     * against a few tens of megabytes of finished model.
     */
    fun purgeIntermediates(projectId: String): Long {
        var freed = 0L
        for (dir in listOf(framesDir(projectId), depthDir(projectId), checkpointsDir(projectId))) {
            freed += directorySize(dir)
            dir.deleteRecursively()
            dir.mkdirs()
        }
        return freed
    }

    /** Remove staged export files; they exist only for the duration of a share. */
    fun clearExports(projectId: String): Long {
        val dir = exportsDir(projectId)
        val freed = directorySize(dir)
        dir.deleteRecursively()
        dir.mkdirs()
        return freed
    }

    /**
     * True once the capture is complete enough to reconstruct: the manifest and at least two
     * frame files are present. Checked before queueing rather than at the start of processing,
     * so the user finds out immediately instead of after the job reaches the front of the queue.
     */
    fun hasUsableCapture(projectId: String): Boolean {
        if (!manifestFile(projectId).isFile) return false
        val frames = framesDir(projectId).listFiles()?.size ?: 0
        return frames >= MIN_FRAMES
    }

    /** Whether a finished model exists, which is what makes a project viewable. */
    fun hasModel(projectId: String): Boolean = outputFile(projectId, MODEL_GLB).isFile

    @Throws(IOException::class)
    fun writeManifest(projectId: String, manifest: CaptureManifest) {
        create(projectId)
        // Write to a temporary file and rename: a manifest truncated by a process kill would
        // make the whole capture unreadable, and rename is atomic within a filesystem.
        val target = manifestFile(projectId)
        val temp = File(target.parentFile, "$MANIFEST.tmp")
        temp.writeText(json.encodeToString(manifest))
        if (!temp.renameTo(target)) {
            temp.delete()
            throw IOException("could not replace ${target.absolutePath}")
        }
    }

    fun readManifest(projectId: String): CaptureManifest? {
        val file = manifestFile(projectId)
        if (!file.isFile) return null
        return runCatching { json.decodeFromString<CaptureManifest>(file.readText()) }.getOrNull()
    }

    /** Usage by category across every project, for the storage dashboard. */
    fun breakdown(): StorageBreakdown {
        var raw = 0L
        var depth = 0L
        var checkpoints = 0L
        var mesh = 0L
        var texture = 0L
        var cloud = 0L
        var splat = 0L
        var thumbnails = 0L
        var exports = 0L
        for (id in listProjectIds()) {
            raw += directorySize(framesDir(id)) + manifestFile(id).lengthOrZero()
            depth += directorySize(depthDir(id))
            checkpoints += directorySize(checkpointsDir(id))
            exports += directorySize(exportsDir(id))
            thumbnails += thumbnailFile(id).lengthOrZero()
            outputsDir(id).listFiles()?.forEach { file ->
                val size = file.length()
                when (classifyOutput(file.name)) {
                    OutputCategory.Texture -> texture += size
                    OutputCategory.PointCloud -> cloud += size
                    OutputCategory.Splat -> splat += size
                    OutputCategory.Mesh -> mesh += size
                }
            }
        }
        return StorageBreakdown(
            rawFramesBytes = raw,
            depthBytes = depth,
            checkpointBytes = checkpoints,
            meshBytes = mesh,
            textureBytes = texture,
            pointCloudBytes = cloud,
            splatBytes = splat,
            thumbnailBytes = thumbnails,
            exportBytes = exports,
            deviceFreeBytes = context.filesDir.usableSpace,
        )
    }

    fun deviceFreeBytes(): Long = context.filesDir.usableSpace

    /**
     * Delete directories with no matching project row.
     *
     * A crash between creating the directory and committing the row would otherwise leave
     * hundreds of megabytes stranded with nothing in the UI referring to it.
     */
    fun deleteOrphans(knownProjectIds: Set<String>): Long {
        var freed = 0L
        for (id in listProjectIds()) {
            if (id in knownProjectIds) continue
            freed += sizeOf(id)
            delete(id)
        }
        return freed
    }

    private fun File.lengthOrZero(): Long = if (isFile) length() else 0L

    companion object {
        const val PROJECTS = "projects"
        const val FRAMES = "frames"
        const val DEPTH = "depth"
        const val CHECKPOINTS = "checkpoints"
        const val OUTPUTS = "outputs"
        const val EXPORTS = "exports"
        const val SHARED = "shared"
        const val MANIFEST = "manifest.json"
        const val THUMBNAIL = "thumbnail.jpg"

        /** Output file names, fixed by the native pipeline. */
        const val MODEL_GLB = "model.glb"
        const val CLOUD_PLY = "cloud.ply"
        const val SPLAT_SPZ = "splat.spz"

        /**
         * The trained splat field, in the reference 3DGS PLY layout.
         *
         * This is the one the pipeline writes and the viewer loads; `.spz` and
         * `.splat` are produced from it on export, because they are lossy and
         * the source should not be.
         */
        const val SPLAT_PLY = "splat.ply"
        const val FLOOR_PLAN_SVG = "floorplan.svg"

        /** Two views is the theoretical minimum for any reconstruction at all. */
        const val MIN_FRAMES = 2

        /**
         * Which storage category an output file belongs to.
         *
         * Floor plans and the texture atlas both land in Texture: they are images, and the
         * dashboard's point is to show the user where the space went, not to itemise formats.
         */
        internal fun classifyOutput(name: String): OutputCategory = when {
            name == CLOUD_PLY -> OutputCategory.PointCloud
            name.endsWith(".spz") || name.endsWith(".splat") -> OutputCategory.Splat
            // A splat PLY is a point set, not a mesh, and is named for what it holds.
            name.startsWith("splat") && name.endsWith(".ply") -> OutputCategory.Splat
            name.endsWith(".png") || name.endsWith(".jpg") || name.endsWith(".svg") ||
                name.endsWith(".pdf") -> OutputCategory.Texture
            else -> OutputCategory.Mesh
        }

        fun directorySize(dir: File): Long {
            if (!dir.exists()) return 0L
            if (dir.isFile) return dir.length()
            var total = 0L
            // An explicit stack rather than recursion: a deeply nested exports directory
            // produced by a third-party unzip should not be able to overflow the stack.
            val stack = ArrayDeque<File>()
            stack.addLast(dir)
            while (stack.isNotEmpty()) {
                val current = stack.removeLast()
                val children = current.listFiles() ?: continue
                for (child in children) {
                    if (child.isDirectory) stack.addLast(child) else total += child.length()
                }
            }
            return total
        }
    }
}

internal enum class OutputCategory { Mesh, Texture, PointCloud, Splat }

/**
 * The capture manifest, in the exact shape the native pipeline parses.
 *
 * The field names are short and lower-case because they are a wire format shared with
 * `Pipeline.cpp`, not Kotlin API: `image`, `depth`, `keyframe`, and an `intrinsics` object with
 * `fx`, `fy`, `cx`, `cy`, `k1`, `k2`, `width`, `height`. Poses are 16 floats, world-from-camera
 * in column-major order.
 */
@Serializable
data class CaptureManifest(
    val version: Int = VERSION,
    val projectId: String,
    val mode: String,
    val createdAt: Long,
    val hasSensorDepth: Boolean = false,
    val hasArPoses: Boolean = false,
    val frames: List<ManifestFrame> = emptyList(),
) {
    companion object {
        const val VERSION = 1

        fun from(
            projectId: String,
            mode: CaptureMode,
            createdAtMillis: Long,
            frames: List<CaptureFrame>,
        ): CaptureManifest = CaptureManifest(
            projectId = projectId,
            mode = mode.name,
            createdAt = createdAtMillis,
            hasSensorDepth = frames.any { it.depthFileName != null },
            hasArPoses = frames.any { it.pose != null },
            frames = frames.map(ManifestFrame::from),
        )
    }
}

@Serializable
data class ManifestFrame(
    val index: Int,
    val image: String,
    val depth: String = "",
    val sharpness: Float = 1f,
    val exposure: Float = 128f,
    val keyframe: Boolean = true,
    val timestamp: Long = 0,
    val intrinsics: ManifestIntrinsics,
    /** Omitted entirely when there is no pose, which is how the pipeline detects Import mode. */
    val pose: List<Float>? = null,
) {
    companion object {
        fun from(frame: CaptureFrame): ManifestFrame = ManifestFrame(
            index = frame.index,
            image = frame.imageFileName,
            depth = frame.depthFileName.orEmpty(),
            sharpness = frame.quality.sharpness,
            exposure = frame.quality.exposure,
            keyframe = frame.isKeyframe,
            timestamp = frame.timestampNanos,
            intrinsics = ManifestIntrinsics(
                fx = frame.intrinsics.focalLengthX,
                fy = frame.intrinsics.focalLengthY,
                cx = frame.intrinsics.principalPointX,
                cy = frame.intrinsics.principalPointY,
                k1 = frame.intrinsics.distortionK1,
                k2 = frame.intrinsics.distortionK2,
                width = frame.intrinsics.imageWidth,
                height = frame.intrinsics.imageHeight,
            ),
            pose = frame.pose?.matrix?.toList(),
        )
    }
}

@Serializable
data class ManifestIntrinsics(
    val fx: Float,
    val fy: Float,
    val cx: Float,
    val cy: Float,
    val k1: Float = 0f,
    val k2: Float = 0f,
    val width: Int,
    val height: Int,
)
