package uk.co.jacoblevy.parallax.engine.nativebridge

import java.nio.ByteBuffer

/**
 * The only entry point to the native reconstruction engine.
 *
 * Every method here is `external`; nothing else in the app touches JNI. The surface is
 * deliberately coarse -- whole pipeline stages, whole files, whole meshes -- because a chatty
 * interface would spend more time crossing the boundary than doing work, and because a thin
 * seam is the only part of the engine that cannot be unit-tested on a host machine, so it is
 * kept as small as possible.
 *
 * Pipelines are referred to by an opaque `Long` handle rather than a pointer. Android can kill
 * and restart this process at any moment; a handle that is simply absent from the native
 * registry afterwards yields a clean error, whereas a stale pointer recovered from saved state
 * is a crash.
 */
object ParallaxEngine {

    /** Thrown when the native library is unavailable, which makes reconstruction impossible. */
    class UnavailableException(cause: Throwable) :
        IllegalStateException("The Parallax reconstruction engine could not be loaded", cause)

    private var loadFailure: Throwable? = null

    val isAvailable: Boolean
        get() {
            ensureLoaded()
            return loadFailure == null
        }

    /** The reason the engine is unavailable, for the diagnostics screen. */
    val unavailableReason: String?
        get() {
            ensureLoaded()
            return loadFailure?.message
        }

    private fun ensureLoaded() {
        if (loaded) return
        synchronized(this) {
            if (loaded) return
            try {
                System.loadLibrary(LIBRARY_NAME)
            } catch (error: UnsatisfiedLinkError) {
                // Recorded rather than rethrown: a device with an unexpected ABI should show a
                // clear "not supported on this device" screen, not crash on first launch.
                loadFailure = error
            } catch (error: SecurityException) {
                loadFailure = error
            }
            loaded = true
        }
    }

    @Volatile
    private var loaded = false

    private const val LIBRARY_NAME = "parallax_engine"

    /** Engine build identifier, for the about screen and crash reports. */
    fun version(): String {
        ensureLoaded()
        loadFailure?.let { return "unavailable" }
        return nativeVersion()
    }

    // ------------------------------------------------------------- pipeline

    /**
     * Create a pipeline over a project directory.
     *
     * @param configJson see [PipelineConfigJson] for the schema
     * @return a handle, or 0 if the configuration was rejected
     */
    fun createPipeline(configJson: String): Long {
        ensureLoaded()
        loadFailure?.let { throw UnavailableException(it) }
        return nativeCreatePipeline(configJson)
    }

    fun destroyPipeline(handle: Long) {
        if (handle == 0L || loadFailure != null) return
        nativeDestroyPipeline(handle)
    }

    /** Ordinal of the next stage to run, or -1 if the handle is unknown. */
    fun nextStage(handle: Long): Int = if (handle == 0L) -1 else nativeNextStage(handle)

    /**
     * Ask the running stage to stop.
     *
     * Cooperative: the engine checks the flag between units of work, so this returns
     * immediately and the stage finishes shortly afterwards reporting `cancelled`.
     */
    fun cancel(handle: Long) {
        if (handle != 0L && loadFailure == null) nativeCancel(handle)
    }

    /** Run one stage. Returns the outcome as JSON; see [StageOutcomeJson]. */
    fun runStage(handle: Long, stage: Int, listener: ProgressListener?): String =
        nativeRunStage(handle, stage, listener)

    /** Run from the next incomplete stage to the end. */
    fun runToCompletion(handle: Long, listener: ProgressListener?): String =
        nativeRunToCompletion(handle, listener)

    /** Discard checkpoints from `stage` onwards so they re-run. */
    fun invalidateFrom(handle: Long, stage: Int) {
        if (handle != 0L && loadFailure == null) nativeInvalidateFrom(handle, stage)
    }

    fun summary(handle: Long): String = if (handle == 0L) "{}" else nativeSummary(handle)

    // --------------------------------------------------------------- export

    /**
     * Write one export.
     *
     * @param format one of `glb`, `gltf`, `obj`, `usdz`, `ply`, `ply_points`, `stl`
     * @param outputPath a file for single-file formats, a directory for `gltf` and `obj`
     */
    fun export(handle: Long, format: String, outputPath: String, optionsJson: String): String =
        nativeExport(handle, format, outputPath, optionsJson)

    // ------------------------------------------------------- live capture

    /**
     * Score a preview frame.
     *
     * @param luma a **direct** ByteBuffer over the camera's Y plane. Direct is required: this
     *   runs on every preview frame, and copying a multi-megapixel plane at 30 Hz would be the
     *   most expensive thing in the app.
     * @return `[sharpness, exposure, contrast, clippedLow, clippedHigh, acceptable]`, or all
     *   zeroes if the buffer was unusable.
     */
    fun scoreFrame(luma: ByteBuffer, width: Int, height: Int, rowStride: Int): DoubleArray {
        require(luma.isDirect) { "scoreFrame needs a direct ByteBuffer" }
        ensureLoaded()
        if (loadFailure != null) return DoubleArray(6)
        return nativeScoreFrame(luma, width, height, rowStride) ?: DoubleArray(6)
    }

    /** Perceptual hash of a preview frame, for near-duplicate rejection. */
    fun frameHash(luma: ByteBuffer, width: Int, height: Int, rowStride: Int): Long {
        require(luma.isDirect) { "frameHash needs a direct ByteBuffer" }
        ensureLoaded()
        if (loadFailure != null) return 0L
        return nativeFrameHash(luma, width, height, rowStride)
    }

    /** Hamming distance between two frame hashes, 0..64. */
    fun hashDistance(a: Long, b: Long): Int {
        ensureLoaded()
        // Cheap enough to do in Kotlin if the library is missing, and it keeps the
        // keyframe-selection logic testable without the engine.
        if (loadFailure != null) return java.lang.Long.bitCount(a xor b)
        return nativeHashDistance(a, b)
    }

    // -------------------------------------------------------------- loading

    /**
     * Finished mesh vertices, interleaved as
     * `position(3), normal(3), uv(2), colour(3)` = 11 floats per vertex.
     * Null when the project has no mesh.
     */
    fun loadMeshVertices(handle: Long): FloatArray? =
        if (handle == 0L) null else nativeLoadMeshVertices(handle)

    /** Triangle indices, three per triangle. Null when the project has no mesh. */
    fun loadMeshIndices(handle: Long): IntArray? =
        if (handle == 0L) null else nativeLoadMeshIndices(handle)

    /** Point cloud interleaved as `position(3), normal(3), colour(3)` = 9 floats per point. */
    fun loadPointCloud(handle: Long): FloatArray? =
        if (handle == 0L) null else nativeLoadPointCloud(handle)

    fun loadFloorPlan(handle: Long): String = if (handle == 0L) "{}" else nativeLoadFloorPlan(handle)

    fun renderFloorPlanSvg(handle: Long, dark: Boolean, imperial: Boolean, title: String): String =
        if (handle == 0L) "" else nativeRenderFloorPlanSvg(handle, dark, imperial, title)

    // --------------------------------------------------------------- splats

    /** Seed a splat cloud from the project's point cloud and write it as a 3DGS PLY. */
    fun initialiseSplats(handle: Long, outputPath: String, shDegree: Int): String =
        if (handle == 0L) "{}" else nativeInitialiseSplats(handle, outputPath, shDegree)

    /** Convert a 3DGS PLY to `splat`, `spz` or `ply`. */
    fun convertSplats(inputPath: String, outputPath: String, format: String): String {
        ensureLoaded()
        loadFailure?.let { throw UnavailableException(it) }
        return nativeConvertSplats(inputPath, outputPath, format)
    }

    // ------------------------------------------------------------- natives
    //
    // Signatures here are matched by name in jni/ParallaxJni.cpp. The ProGuard rules in
    // engine/native/consumer-rules.pro keep this class and its native methods from being
    // renamed; without them a release build links against nothing.

    @JvmStatic private external fun nativeVersion(): String
    @JvmStatic private external fun nativeCreatePipeline(configJson: String): Long
    @JvmStatic private external fun nativeDestroyPipeline(handle: Long)
    @JvmStatic private external fun nativeNextStage(handle: Long): Int
    @JvmStatic private external fun nativeCancel(handle: Long)
    @JvmStatic private external fun nativeRunStage(
        handle: Long,
        stage: Int,
        listener: ProgressListener?,
    ): String
    @JvmStatic private external fun nativeRunToCompletion(
        handle: Long,
        listener: ProgressListener?,
    ): String
    @JvmStatic private external fun nativeInvalidateFrom(handle: Long, stage: Int)
    @JvmStatic private external fun nativeSummary(handle: Long): String
    @JvmStatic private external fun nativeExport(
        handle: Long,
        format: String,
        outputPath: String,
        optionsJson: String,
    ): String
    @JvmStatic private external fun nativeScoreFrame(
        luma: ByteBuffer,
        width: Int,
        height: Int,
        rowStride: Int,
    ): DoubleArray?
    @JvmStatic private external fun nativeFrameHash(
        luma: ByteBuffer,
        width: Int,
        height: Int,
        rowStride: Int,
    ): Long
    @JvmStatic private external fun nativeHashDistance(a: Long, b: Long): Int
    @JvmStatic private external fun nativeLoadMeshVertices(handle: Long): FloatArray?
    @JvmStatic private external fun nativeLoadMeshIndices(handle: Long): IntArray?
    @JvmStatic private external fun nativeLoadPointCloud(handle: Long): FloatArray?
    @JvmStatic private external fun nativeLoadFloorPlan(handle: Long): String
    @JvmStatic private external fun nativeRenderFloorPlanSvg(
        handle: Long,
        dark: Boolean,
        imperial: Boolean,
        title: String,
    ): String
    @JvmStatic private external fun nativeInitialiseSplats(
        handle: Long,
        outputPath: String,
        shDegree: Int,
    ): String
    @JvmStatic private external fun nativeConvertSplats(
        inputPath: String,
        outputPath: String,
        format: String,
    ): String
}

/**
 * Progress callback invoked from the native thread running the stage.
 *
 * An interface rather than a Kotlin lambda because JNI resolves `onProgress` by name and
 * signature; a `Function4` would be obfuscated and its bridge method is not stable.
 *
 * Implementations must be cheap and must not block: they run on the reconstruction thread, and
 * anything slow here directly slows the scan down.
 */
interface ProgressListener {
    /**
     * @param stage ordinal matching `ReconstructionStage`
     * @param stageFraction 0..1 within the current stage
     * @param overallFraction 0..1 across the whole pipeline
     * @param detail a short human-readable note, already localised to English
     */
    fun onProgress(stage: Int, stageFraction: Double, overallFraction: Double, detail: String)
}
