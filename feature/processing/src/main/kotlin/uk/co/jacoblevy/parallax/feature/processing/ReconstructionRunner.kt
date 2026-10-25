package uk.co.jacoblevy.parallax.feature.processing

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import uk.co.jacoblevy.parallax.core.common.di.ReconstructionDispatcher
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.BoundingBox
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.ModelMeasurements
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ReconstructionStage
import uk.co.jacoblevy.parallax.engine.nativebridge.ParallaxEngine
import uk.co.jacoblevy.parallax.engine.nativebridge.ProgressListener
import javax.inject.Inject
import javax.inject.Singleton

/** Progress from the native pipeline, as the UI and the notification need it. */
data class ReconstructionProgress(
    val stage: ReconstructionStage,
    val stageFraction: Float,
    val overallFraction: Float,
    val detail: String,
    val etaMillis: Long?,
)

/** How a run ended. */
sealed interface ReconstructionOutcome {
    data class Success(
        val outputs: Set<OutputKind>,
        val measurements: ModelMeasurements,
    ) : ReconstructionOutcome

    data class Failed(val error: ParallaxError, val resumableFrom: ReconstructionStage?) :
        ReconstructionOutcome

    data object Cancelled : ReconstructionOutcome
}

/**
 * Runs the native reconstruction pipeline for one project.
 *
 * Stage by stage rather than in one call, for two reasons: the project's status in the database
 * is updated between stages so the library and the notification stay truthful, and a run that
 * dies mid-stage resumes from the last completed one rather than starting over. The engine
 * checkpoints each stage to disk, which is what makes that possible -- Android will kill a
 * foreground service eventually, and a twenty-minute reconstruction that has to restart is one
 * nobody will wait for twice.
 */
@Singleton
class ReconstructionRunner @Inject constructor(
    private val storage: ProjectStorage,
    @ReconstructionDispatcher private val dispatcher: CoroutineDispatcher,
) {
    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

    /**
     * Run [project] to completion, or until [shouldCancel] returns true.
     *
     * @param onProgress called from the reconstruction dispatcher, frequently. Must be cheap.
     */
    suspend fun run(
        project: Project,
        capabilities: DeviceCapabilities,
        shouldCancel: () -> Boolean,
        onProgress: (ReconstructionProgress) -> Unit,
    ): ReconstructionOutcome = withContext(dispatcher) {
        ParallaxEngine.unavailableReason?.let { reason ->
            return@withContext ReconstructionOutcome.Failed(
                error = ParallaxError.EngineFailure("load", reason),
                resumableFrom = null,
            )
        }

        val configJson = json.encodeToString(configFor(project, capabilities))
        val handle = ParallaxEngine.createPipeline(configJson)
        if (handle == 0L) {
            return@withContext ReconstructionOutcome.Failed(
                error = ParallaxError.EngineFailure(
                    "createPipeline",
                    "the project directory could not be opened",
                ),
                resumableFrom = null,
            )
        }

        try {
            runStages(handle, project, shouldCancel, onProgress)
        } finally {
            ParallaxEngine.destroyPipeline(handle)
        }
    }

    private fun runStages(
        handle: Long,
        project: Project,
        shouldCancel: () -> Boolean,
        onProgress: (ReconstructionProgress) -> Unit,
    ): ReconstructionOutcome {
        val startedAt = System.currentTimeMillis()
        val stages = ReconstructionStage.entries
        var next = ParallaxEngine.nextStage(handle)
        if (next < 0) {
            return ReconstructionOutcome.Failed(
                error = ParallaxError.EngineFailure("nextStage", "the pipeline is not usable"),
                resumableFrom = null,
            )
        }

        while (next in stages.indices) {
            if (shouldCancel()) {
                ParallaxEngine.cancel(handle)
                return ReconstructionOutcome.Cancelled
            }
            val currentStage = stages[next]

            val listener = object : ProgressListener {
                override fun onProgress(
                    stage: Int,
                    stageFraction: Double,
                    overallFraction: Double,
                    detail: String,
                ) {
                    // Cancellation is checked here as well as between stages, because a single
                    // stage can run for minutes and the engine's own cancel flag is the only
                    // thing that can stop it part-way.
                    if (shouldCancel()) ParallaxEngine.cancel(handle)
                    onProgress(
                        ReconstructionProgress(
                            stage = stages.getOrElse(stage) { currentStage },
                            stageFraction = stageFraction.toFloat(),
                            overallFraction = overallFraction.toFloat(),
                            detail = detail,
                            etaMillis = estimateRemaining(startedAt, overallFraction),
                        ),
                    )
                }
            }

            val outcome = json.decodeFromString<StageResult>(
                ParallaxEngine.runStage(handle, next, listener),
            )
            if (outcome.cancelled) return ReconstructionOutcome.Cancelled
            if (!outcome.success) {
                return ReconstructionOutcome.Failed(
                    error = ParallaxError.ProcessingFailed(
                        stage = currentStage.label,
                        detail = outcome.message,
                        // Everything up to this stage is checkpointed, so a retry resumes here
                        // rather than from the beginning.
                        resumableFromStage = currentStage.name,
                    ),
                    resumableFrom = currentStage,
                )
            }
            if (outcome.complete) {
                return success(project, outcome.summary)
            }
            next = outcome.nextStage
        }

        // Already complete when the run started: a resumed job whose work was all done.
        return success(project, json.decodeFromString<PipelineSummaryJson>(
            ParallaxEngine.summary(handle),
        ))
    }

    private fun success(project: Project, summary: PipelineSummaryJson?): ReconstructionOutcome {
        val resolved = summary ?: PipelineSummaryJson()
        val outputs = buildSet {
            if (resolved.triangleCount > 0) {
                add(OutputKind.Mesh)
                if (resolved.hasTexture) add(OutputKind.TexturedMesh)
                if (resolved.watertight) add(OutputKind.WatertightMesh)
            }
            if (resolved.pointCount > 0) add(OutputKind.PointCloud)
            if (resolved.hasFloorPlan) add(OutputKind.FloorPlan)
            if (resolved.splatCount > 0 ||
                storage.outputFile(project.id, ProjectStorage.SPLAT_PLY).isFile
            ) {
                add(OutputKind.Splat)
            }
        }
        if (outputs.isEmpty()) {
            return ReconstructionOutcome.Failed(
                error = ParallaxError.ReconstructionEmpty(ReconstructionStage.Finalise.label),
                resumableFrom = ReconstructionStage.Fusion,
            )
        }
        return ReconstructionOutcome.Success(
            outputs = outputs,
            measurements = ModelMeasurements(
                triangleCount = resolved.triangleCount,
                vertexCount = resolved.vertexCount,
                pointCount = resolved.pointCount,
                splatCount = resolved.splatCount,
                surfaceAreaSquareMetres = resolved.surfaceAreaM2,
                volumeCubicMetres = resolved.volumeM3,
                boundsMetres = resolved.bounds.toBoundingBox(),
                watertight = resolved.watertight,
                floorAreaSquareMetres = resolved.floorAreaM2.takeIf { it > 0.0 },
                ceilingHeightMetres = resolved.ceilingHeightM.takeIf { it > 0.0 },
            ),
        )
    }

    /**
     * Remaining time from elapsed time and progress so far.
     *
     * Linear extrapolation, which is honest about what it is: the stage weights in
     * [ReconstructionStage] are what make the progress fraction advance at a roughly constant
     * rate, so a linear estimate over that fraction is about as good as anything more clever.
     * Returns null before there is enough progress to extrapolate from, because an ETA computed
     * from 1% of a run is noise.
     */
    private fun estimateRemaining(startedAtMillis: Long, overallFraction: Double): Long? {
        if (overallFraction < MIN_FRACTION_FOR_ETA) return null
        val elapsed = System.currentTimeMillis() - startedAtMillis
        if (elapsed <= 0) return null
        val total = elapsed / overallFraction
        return (total - elapsed).toLong().coerceAtLeast(0)
    }

    /**
     * The configuration this project would be run with.
     *
     * Exposed for tests: the native engine cannot run on a JVM host, but the configuration is
     * where the per-mode decisions live -- whether to cut the ground away, how far to
     * integrate, what the atlas can hold -- and getting one of those wrong produces a
     * reconstruction that is subtly wrong rather than one that fails.
     */
    @androidx.annotation.VisibleForTesting
    internal fun configForTesting(
        project: Project,
        capabilities: DeviceCapabilities,
    ): PipelineConfigJson = configFor(project, capabilities)

    /** The native pipeline's configuration for this project. */
    private fun configFor(project: Project, capabilities: DeviceCapabilities): PipelineConfigJson {
        val preset = project.quality
        val mode = project.captureMode
        return PipelineConfigJson(
            projectDir = storage.projectDir(project.id).absolutePath,
            mode = when (mode) {
                CaptureMode.Object -> "object"
                CaptureMode.Room -> "room"
                CaptureMode.Area -> "area"
                CaptureMode.Import -> "import"
            },
            voxelSize = preset.voxelSizeMetres,
            maxKeyframes = preset.maxKeyframes,
            textureResolution = minOf(preset.textureResolution, capabilities.maxTextureSize),
            targetTriangles = targetTrianglesFor(preset, capabilities),
            hasDevicePoses = project.capture.hasArPoses,
            hasDepthMaps = project.capture.hasSensorDepth,
            memoryBudgetBytes = capabilities.reconstructionMemoryBudgetBytes,
            maxIntegrationDepth = maxIntegrationDepthFor(mode),
            // Object mode cuts away the supporting surface and keeps only the subject; doing
            // that to a room would delete the floor and then the room.
            removeGroundPlane = mode == CaptureMode.Object,
            keepLargestComponent = mode == CaptureMode.Object,
            autoLevel = true,
            buildPointCloud = true,
            buildWatertight = true,
            analyseRoom = mode == CaptureMode.Room,
            trainSplats = preset.runSplatTraining && capabilities.canTrainSplats,
            splatIterations = preset.splatIterations,
            splatMaxGaussians = splatBudgetFor(preset, capabilities),
            // Degree two is where view-dependent shading stops paying for
            // itself on a phone: degree three nearly doubles the parameter
            // count for highlights a hand-held capture rarely resolves.
            splatShDegree = if (capabilities.splatsTrainOnGpu) 2 else 1,
            splatImageWidth = SPLAT_TRAINING_WIDTH,
            splatMaxViews = SPLAT_MAX_TRAINING_VIEWS,
            splatTimeBudgetMillis = SPLAT_TIME_BUDGET_MILLIS,
        )
    }

    /**
     * How many gaussians this device can hold.
     *
     * The preset says what the quality level wants; the device's splat memory
     * budget says what it can have. The smaller wins, with a floor below which
     * a splat field is not worth producing at all.
     */
    private fun splatBudgetFor(preset: QualityPreset, capabilities: DeviceCapabilities): Int {
        if (preset.splatMaxGaussians <= 0) return 0
        val affordable = capabilities.splatMemoryBudgetBytes / SPLAT_BYTES_EACH
        return affordable
            .coerceAtMost(preset.splatMaxGaussians.toLong())
            .coerceAtLeast(MIN_GAUSSIANS.toLong())
            .toInt()
    }

    /**
     * The triangle budget, capped by what the atlas can texture.
     *
     * Each triangle needs its own patch in the texture atlas, so a target the atlas cannot hold
     * produces a mesh whose later triangles are left untextured. The cap keeps the two in step.
     */
    private fun targetTrianglesFor(preset: QualityPreset, capabilities: DeviceCapabilities): Int {
        val atlas = minOf(preset.textureResolution, capabilities.maxTextureSize)
        val patchesPerSide = atlas / MIN_PATCH_PIXELS
        val textureCap = (patchesPerSide * patchesPerSide * ATLAS_PACKING_EFFICIENCY).toInt()
        return minOf(preset.targetTriangles, textureCap).coerceAtLeast(MIN_TRIANGLES)
    }

    /**
     * How far to integrate depth, per mode.
     *
     * This is a quality setting, not a performance one. The voxel block budget is finite, and
     * integrating the floor stretching away behind a small object spends the whole budget on
     * surface nobody wants -- leaving none for the subject, so the extraction comes back empty.
     */
    private fun maxIntegrationDepthFor(mode: CaptureMode): Float = when (mode) {
        CaptureMode.Object -> OBJECT_INTEGRATION_DEPTH_M
        CaptureMode.Room -> ROOM_INTEGRATION_DEPTH_M
        CaptureMode.Area -> AREA_INTEGRATION_DEPTH_M
        CaptureMode.Import -> IMPORT_INTEGRATION_DEPTH_M
    }

    private companion object {
        const val MIN_FRACTION_FOR_ETA = 0.05
        const val MIN_PATCH_PIXELS = 6
        const val ATLAS_PACKING_EFFICIENCY = 0.7
        const val MIN_TRIANGLES = 5_000

        /**
         * How far to integrate, per mode. An orbit of a small object is a metre and a half of
         * useful depth; an area scan genuinely needs twelve.
         */
        const val OBJECT_INTEGRATION_DEPTH_M = 1.6f
        const val ROOM_INTEGRATION_DEPTH_M = 6f
        const val AREA_INTEGRATION_DEPTH_M = 12f
        const val IMPORT_INTEGRATION_DEPTH_M = 5f

        /**
         * Width the training views are downscaled to.
         *
         * A splat field resolves detail from having many views rather than
         * large ones, and full-resolution frames cost memory the optimiser
         * needs for gaussians. 512 is about where more pixels stop improving
         * the result on a hand-held capture.
         */
        const val SPLAT_TRAINING_WIDTH = 512
        const val SPLAT_MAX_TRAINING_VIEWS = 80

        /**
         * Longest splat training may run before it keeps what it has and stops.
         *
         * Iterations are not a budget on a phone. The same count is seconds of GPU compute and
         * hours of CPU work with sixty thousand gaussians, and a real device sat at 83% for
         * hours -- with the model it had already built unopenable, because the stage before
         * finalise had not returned. Four minutes is long enough for the field to be clearly
         * better than the seeding and short enough that nobody watches it.
         *
         * The field is a refinement of a model that already exists, so stopping early costs
         * sharpness in the splat view and nothing else.
         */
        const val SPLAT_TIME_BUDGET_MILLIS = 4L * 60L * 1000L

        /**
         * Bytes the optimiser holds per gaussian.
         *
         * At degree two a gaussian is 38 floats, and training keeps four
         * copies of each: the value, its gradient and both Adam moments. That
         * is 608 bytes, plus about 150 for the projected state, the
         * screen-space gradient accumulator and the splat's share of the sort
         * keys.
         */
        const val SPLAT_BYTES_EACH = 760
        const val MIN_GAUSSIANS = 20_000
    }
}

// ---- the JSON the JNI bridge speaks ---------------------------------------

@Serializable
internal data class PipelineConfigJson(
    val projectDir: String,
    val mode: String,
    val voxelSize: Float,
    val maxKeyframes: Int,
    val textureResolution: Int,
    val targetTriangles: Int,
    val hasDevicePoses: Boolean,
    val hasDepthMaps: Boolean,
    val memoryBudgetBytes: Long,
    val maxIntegrationDepth: Float,
    val removeGroundPlane: Boolean,
    val keepLargestComponent: Boolean,
    val autoLevel: Boolean,
    val buildPointCloud: Boolean,
    val buildWatertight: Boolean,
    val analyseRoom: Boolean,
    val truncationMultiplier: Float = 4f,
    val trainSplats: Boolean = false,
    val splatIterations: Int = 5_000,
    val splatMaxGaussians: Int = 180_000,
    val splatShDegree: Int = 2,
    val splatImageWidth: Int = 512,
    val splatMaxViews: Int = 80,
    val splatTimeBudgetMillis: Long = 0,
)

@Serializable
internal data class StageResult(
    val success: Boolean = false,
    val cancelled: Boolean = false,
    val message: String = "",
    val nextStage: Int = -1,
    val complete: Boolean = false,
    val summary: PipelineSummaryJson? = null,
)

@Serializable
internal data class PipelineSummaryJson(
    val frameCount: Int = 0,
    val keyframeCount: Int = 0,
    val registeredFrameCount: Int = 0,
    val meanReprojectionErrorPx: Double = 0.0,
    val triangleCount: Long = 0,
    val vertexCount: Long = 0,
    val pointCount: Long = 0,
    val splatCount: Long = 0,
    /** Which backend trained the splats: "gpu", "cpu", or empty if none did. */
    val splatBackend: String = "",
    val surfaceAreaM2: Double = 0.0,
    val volumeM3: Double = 0.0,
    val watertight: Boolean = false,
    val floorAreaM2: Double = 0.0,
    val ceilingHeightM: Double = 0.0,
    val hasFloorPlan: Boolean = false,
    val hasTexture: Boolean = false,
    val scaleFactor: Double = 1.0,
    /**
     * The bounding box, as the six numbers the bridge emits: min x, y, z then max x, y, z.
     *
     * An array rather than an object because that is what `ParallaxJni.cpp` writes, and the
     * two have to agree exactly -- a mismatch here is a silently empty bounding box, which the
     * viewer would read as a model with no extent.
     */
    val bounds: List<Float>? = null,
)

/** A bounding box is six numbers: the minimum corner then the maximum corner. */
private const val BOUNDS_ELEMENTS = 6

/** The six-element bounds array, or an empty box when it is absent or malformed. */
internal fun List<Float>?.toBoundingBox(): BoundingBox {
    if (this == null || size < BOUNDS_ELEMENTS) return BoundingBox()
    return BoundingBox(
        minX = this[0],
        minY = this[1],
        minZ = this[2],
        maxX = this[3],
        maxY = this[4],
        maxZ = this[5],
    )
}
