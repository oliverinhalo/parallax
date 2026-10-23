package uk.co.jacoblevy.parallax.core.model

import kotlinx.serialization.Serializable

/** A single scan, from capture through processing to export. */
@Serializable
data class Project(
    val id: String,
    val name: String,
    val captureMode: CaptureMode,
    val createdAtMillis: Long,
    val updatedAtMillis: Long,
    val status: ProjectStatus,
    val folderId: String? = null,
    val quality: QualityPreset = QualityPreset.Balanced,
    val capture: CaptureSummary = CaptureSummary(),
    val outputs: Set<OutputKind> = emptySet(),
    val measurements: ModelMeasurements? = null,
    val storageBytes: Long = 0,
    /** Set when the project is starred, which pins it to the top of the library. */
    val favourite: Boolean = false,
    val notes: String = "",
) {
    val isProcessed: Boolean get() = status is ProjectStatus.Ready
    val canProcess: Boolean
        get() = status is ProjectStatus.Captured || status is ProjectStatus.Failed

    /** True once there is something the viewer can show. */
    val hasViewableOutput: Boolean
        get() = outputs.any { it == OutputKind.Mesh || it == OutputKind.PointCloud || it == OutputKind.Splat }
}

@Serializable
enum class CaptureMode {
    /** Orbit a single object. Produces a cut-out model with the ground removed. */
    Object,

    /** Walk around an interior. Produces a model plus a measured floor plan. */
    Room,

    /** Large outdoor or open space. Chunked processing, drift handling. */
    Area,

    /** Built from existing photos or video, with no device poses available. */
    Import,
    ;

    val requiresArCore: Boolean get() = this != Import
}

@Serializable
sealed interface ProjectStatus {
    /** Capture has started but not finished; the session can be resumed. */
    @Serializable
    data class Capturing(val frameCount: Int) : ProjectStatus

    /** Capture finished, nothing processed yet. */
    @Serializable
    data object Captured : ProjectStatus

    /** Waiting in the processing queue. */
    @Serializable
    data class Queued(val position: Int) : ProjectStatus

    @Serializable
    data class Processing(
        val stage: ReconstructionStage,
        val stageProgress: Float,
        val overallProgress: Float,
        val etaMillis: Long? = null,
    ) : ProjectStatus

    /** Processing finished. Splat training may still be refining in the background. */
    @Serializable
    data class Ready(val refiningSplats: Boolean = false) : ProjectStatus

    @Serializable
    data class Failed(
        val stage: ReconstructionStage,
        val reason: String,
        val resumableFrom: ReconstructionStage?,
    ) : ProjectStatus

    @Serializable
    data object Cancelled : ProjectStatus
}

/**
 * The reconstruction pipeline's stages, in order.
 *
 * Each stage checkpoints its output to disk, so a process death or a cancellation resumes from
 * the last completed stage instead of starting over. [weight] is the stage's share of the total
 * runtime, measured on a mid-range device, and is what makes the overall progress bar advance at
 * a roughly constant rate instead of sitting at 40% for three minutes.
 */
@Serializable
enum class ReconstructionStage(val label: String, val weight: Float) {
    Ingest("Reading frames", 0.04f),
    PoseRefinement("Refining camera positions", 0.16f),
    Depth("Estimating depth", 0.20f),
    Fusion("Fusing surfaces", 0.18f),
    Meshing("Building the mesh", 0.10f),
    MeshCleanup("Cleaning up geometry", 0.06f),
    Texturing("Painting textures", 0.14f),
    PointCloud("Building the point cloud", 0.04f),
    RoomAnalysis("Measuring the room", 0.03f),
    Finalise("Finishing up", 0.05f),
    ;

    companion object {
        /** Fraction of total work completed once [stage] has finished. */
        fun cumulativeWeight(stage: ReconstructionStage): Float {
            var sum = 0f
            for (entry in entries) {
                sum += entry.weight
                if (entry == stage) break
            }
            return sum.coerceIn(0f, 1f)
        }

        /** Overall progress given the current stage and its own progress. */
        fun overallProgress(stage: ReconstructionStage, stageProgress: Float): Float {
            val before = cumulativeWeight(stage) - stage.weight
            return (before + stage.weight * stageProgress.coerceIn(0f, 1f)).coerceIn(0f, 1f)
        }

        fun next(stage: ReconstructionStage): ReconstructionStage? =
            entries.getOrNull(stage.ordinal + 1)
    }
}

/** What a finished project can offer. */
@Serializable
enum class OutputKind {
    Mesh,
    TexturedMesh,
    PointCloud,
    Splat,
    FloorPlan,
    WatertightMesh,
}

/**
 * Quality presets. The numbers here are the actual knobs the pipeline reads, kept in the domain
 * model so the "estimated time and storage" shown next to each preset is derived from the same
 * values that drive the work, rather than being a separate hard-coded guess.
 */
@Serializable
enum class QualityPreset(
    val label: String,
    val voxelSizeMetres: Float,
    val maxKeyframes: Int,
    val textureResolution: Int,
    val targetTriangles: Int,
    val runSplatTraining: Boolean,
    /** Rough seconds per keyframe on a mid-range 2024 device. Used for the ETA. */
    val secondsPerKeyframe: Float,
) {
    Fast(
        label = "Fast",
        voxelSizeMetres = 0.015f,
        maxKeyframes = 80,
        textureResolution = 2048,
        targetTriangles = 80_000,
        runSplatTraining = false,
        secondsPerKeyframe = 0.9f,
    ),
    Balanced(
        label = "Balanced",
        voxelSizeMetres = 0.008f,
        maxKeyframes = 160,
        textureResolution = 4096,
        targetTriangles = 300_000,
        runSplatTraining = true,
        secondsPerKeyframe = 2.1f,
    ),
    Max(
        label = "Max",
        voxelSizeMetres = 0.004f,
        maxKeyframes = 300,
        textureResolution = 8192,
        targetTriangles = 1_200_000,
        runSplatTraining = true,
        secondsPerKeyframe = 5.4f,
    ),
    ;

    /** Estimated processing time for a capture with [keyframes] frames. */
    fun estimatedMillis(keyframes: Int): Long =
        (keyframes.coerceAtMost(maxKeyframes) * secondsPerKeyframe * 1000).toLong()

    /**
     * Estimated peak storage. Dominated by the TSDF volume, whose footprint scales with the
     * inverse cube of the voxel size, plus the texture atlas.
     */
    fun estimatedBytes(keyframes: Int, sceneExtentMetres: Float): Long {
        val voxels = (sceneExtentMetres / voxelSizeMetres).toDouble()
        // Only a shell of the volume is ever allocated, so the surface area term dominates.
        val volumeBytes = (voxels * voxels * 12.0 * 16.0).toLong()
        val textureBytes = textureResolution.toLong() * textureResolution * 3
        val frameBytes = keyframes.toLong() * 2_500_000
        return volumeBytes + textureBytes + frameBytes
    }
}

/** What capture produced, summarised for the library and the processing estimate. */
@Serializable
data class CaptureSummary(
    val frameCount: Int = 0,
    val keyframeCount: Int = 0,
    val rejectedFrameCount: Int = 0,
    val durationMillis: Long = 0,
    val coverage: Float = 0f,
    val hasSensorDepth: Boolean = false,
    val hasArPoses: Boolean = false,
    val sceneExtentMetres: Float = 0f,
    val meanSharpness: Float = 0f,
    /** Estimated quality of the model this capture can produce, 0..1. */
    val predictedQuality: Float = 0f,
)

/** Measurements of a finished model, shown in the viewer's info panel. */
@Serializable
data class ModelMeasurements(
    val triangleCount: Long = 0,
    val vertexCount: Long = 0,
    val pointCount: Long = 0,
    val splatCount: Long = 0,
    val surfaceAreaSquareMetres: Double = 0.0,
    val volumeCubicMetres: Double = 0.0,
    val boundsMetres: BoundingBox = BoundingBox(),
    val watertight: Boolean = false,
    val floorAreaSquareMetres: Double? = null,
    val ceilingHeightMetres: Double? = null,
)

@Serializable
data class BoundingBox(
    val minX: Float = 0f,
    val minY: Float = 0f,
    val minZ: Float = 0f,
    val maxX: Float = 0f,
    val maxY: Float = 0f,
    val maxZ: Float = 0f,
) {
    val width: Float get() = maxX - minX
    val height: Float get() = maxY - minY
    val depth: Float get() = maxZ - minZ
    val diagonal: Float
        get() = kotlin.math.sqrt(width * width + height * height + depth * depth)
    val isEmpty: Boolean get() = maxX < minX || maxY < minY || maxZ < minZ
}

/** A user-created folder in the library. */
@Serializable
data class Folder(
    val id: String,
    val name: String,
    val createdAtMillis: Long,
    val projectCount: Int = 0,
)
