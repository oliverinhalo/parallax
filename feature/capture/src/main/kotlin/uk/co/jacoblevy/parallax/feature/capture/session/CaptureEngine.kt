package uk.co.jacoblevy.parallax.feature.capture.session

import android.media.Image
import uk.co.jacoblevy.parallax.core.model.CameraIntrinsics
import uk.co.jacoblevy.parallax.core.model.CaptureFrame
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.CaptureStats
import uk.co.jacoblevy.parallax.core.model.CaptureSummary
import uk.co.jacoblevy.parallax.core.model.CoverageMap
import uk.co.jacoblevy.parallax.core.model.DetectedPlane
import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.GuidancePrompt
import uk.co.jacoblevy.parallax.core.model.PlaneKind
import uk.co.jacoblevy.parallax.core.model.Pose
import uk.co.jacoblevy.parallax.core.model.TrackingState
import uk.co.jacoblevy.parallax.engine.nativebridge.ParallaxEngine
import uk.co.jacoblevy.parallax.feature.capture.logic.CaptureBudget
import uk.co.jacoblevy.parallax.feature.capture.logic.CoverageGap
import uk.co.jacoblevy.parallax.feature.capture.logic.CoverageTracker
import uk.co.jacoblevy.parallax.feature.capture.logic.GuidanceEngine
import uk.co.jacoblevy.parallax.feature.capture.logic.GuidanceInputs
import uk.co.jacoblevy.parallax.feature.capture.logic.GuidanceSession
import uk.co.jacoblevy.parallax.feature.capture.logic.KeyframeDecision
import uk.co.jacoblevy.parallax.feature.capture.logic.KeyframeSelector
import uk.co.jacoblevy.parallax.feature.capture.logic.speedBetween
import java.nio.ByteBuffer

/** What one processed frame changed, for the UI to draw. */
data class CaptureTick(
    val stats: CaptureStats,
    val coverage: CoverageMap,
    val prompt: GuidancePrompt?,
    /** True when the prompt changed, which is when a haptic should fire. */
    val promptChanged: Boolean,
    val recorded: CaptureFrame?,
    val planes: List<DetectedPlane> = emptyList(),
)

/**
 * The capture loop's decision-making, with the camera and the filesystem behind interfaces.
 *
 * This is where frame scoring, keyframe selection, coverage and guidance meet. It is a plain
 * class driven by [onFrame] rather than a coroutine of its own, because the caller already has a
 * thread with the right affinity -- ARCore's frames must be consumed on the GL thread that owns
 * the session -- and introducing a second one would mean copying every frame to hand it over.
 */
class CaptureEngine(
    private val mode: CaptureMode,
    private val budget: CaptureBudget,
    private val recorder: FrameRecorder,
    private val coverage: CoverageTracker = CoverageTracker(),
    private val keyframes: KeyframeSelector = KeyframeSelector(),
    private val guidance: GuidanceSession = GuidanceSession(),
    /** Injectable so tests do not depend on a loaded native library. */
    private val scorer: FrameScorer = NativeFrameScorer,
) {
    private var frameIndex = 0
    private var rejected = 0
    private var startedAtMillis = 0L
    private var lastKeyframeMillis = 0L
    private var lastPose: Pose? = null
    private var lastPoseNanos = 0L
    private var recentRejections = ArrayDeque<Boolean>()
    private var conditions = DeviceConditions()
    private var subjectDistance: Float? = null
    private var planes: List<DetectedPlane> = emptyList()
    private var lastHash = 0L

    val keyframeCount: Int get() = recorder.frameCount
    val frames: List<CaptureFrame> get() = recorder.frames

    /**
     * Begin a capture session.
     *
     * Anything a previous session wrote into the same project is discarded. Without that, a
     * user who backs out of a scan and starts again would silently reconstruct both attempts
     * together -- two half-orbits from different starting positions, which produces a worse
     * model than either alone and gives no clue why.
     */
    fun start(nowMillis: Long) {
        recorder.discard()
        recorder.prepare()
        startedAtMillis = nowMillis
        lastKeyframeMillis = 0L
        frameIndex = 0
        rejected = 0
        recentRejections.clear()
        coverage.reset()
        keyframes.reset()
        guidance.reset()
        lastPose = null
    }

    fun updateConditions(conditions: DeviceConditions) {
        this.conditions = conditions
    }

    fun updatePlanes(planes: List<DetectedPlane>) {
        this.planes = planes
        // In object mode the subject sits on the largest upward-facing plane, and the ground
        // cutout needs that plane's height. In room mode the same plane is the floor.
        if (!coverage.hasCentre) {
            val floor = planes
                .filter { it.kind == PlaneKind.HorizontalUpward }
                .maxByOrNull { it.extentX * it.extentZ }
            if (floor != null && mode == CaptureMode.Object) {
                coverage.setCentre(floor.centerX, floor.centerY, floor.centerZ)
            }
        }
    }

    /** The point the capture is orbiting, set by a tap or from the first plane hit. */
    fun setSubjectCentre(x: Float, y: Float, z: Float) {
        coverage.setCentre(x, y, z)
    }

    fun setSubjectDistance(metres: Float?) {
        subjectDistance = metres
    }

    /**
     * Process one camera frame.
     *
     * @param luma a direct [ByteBuffer] over the Y plane, which the native scorer reads without
     *   copying. The caller still owns [image] and must close it.
     */
    @Suppress("LongParameterList")
    fun onFrame(
        nowMillis: Long,
        timestampNanos: Long,
        trackingState: TrackingState,
        pose: Pose?,
        intrinsics: CameraIntrinsics?,
        luma: ByteBuffer,
        lumaWidth: Int,
        lumaHeight: Int,
        lumaRowStride: Int,
        image: Image?,
        depth: Image?,
    ): CaptureTick {
        val quality = scorer.score(luma, lumaWidth, lumaHeight, lumaRowStride)
        val hash = scorer.hash(luma, lumaWidth, lumaHeight, lumaRowStride)

        val speed = computeSpeed(pose, timestampNanos)
        val coverageGain = pose?.let { coverage.record(it) } ?: 0f

        val inputs = GuidanceInputs(
            mode = mode,
            trackingState = trackingState,
            quality = quality,
            coverage = coverage.coverage,
            coverageGap = coverage.largestGap(),
            speedMetresPerSecond = speed,
            subjectDistanceMetres = subjectDistance,
            thermalStatus = conditions.thermalStatus,
            batteryPercent = conditions.batteryPercent,
            storageAvailableBytes = conditions.storageAvailableBytes,
            keyframeCount = recorder.frameCount,
            recentRejectionRate = recentRejectionRate(),
            // `onFrame` is only called while the shutter is down, so reaching here is
            // recording; the flag exists so the guidance rules can say so explicitly.
            recording = startedAtMillis > 0,
            millisSinceKeyframe = millisSinceKeyframe(nowMillis),
        )
        val promptChanged = guidance.update(inputs, nowMillis)

        var recorded: CaptureFrame? = null
        if (shouldAttempt(inputs, nowMillis) && intrinsics != null) {
            val decision = keyframes.offer(
                pose = pose,
                quality = quality,
                imageHash = hash,
                hashDistance = scorer.distance(hash, lastHash),
                coverageGain = coverageGain,
            )
            if (decision is KeyframeDecision.Keep) {
                recorded = recorder.record(
                    index = frameIndex++,
                    image = image,
                    depth = depth,
                    pose = pose,
                    intrinsics = intrinsics,
                    quality = quality,
                    imageHash = hash,
                    isKeyframe = true,
                    trackingState = trackingState,
                    timestampNanos = timestampNanos,
                )
                lastKeyframeMillis = nowMillis
                lastHash = hash
                noteRejection(false)
            } else {
                noteRejection(true)
            }
        } else if (!quality.acceptable) {
            noteRejection(true)
        }

        return CaptureTick(
            stats = stats(nowMillis, quality, trackingState, speed),
            coverage = coverage.snapshot(),
            prompt = guidance.prompt,
            promptChanged = promptChanged,
            recorded = recorded,
            planes = planes,
        )
    }

    /** What capture produced, for the project record. */
    fun summarise(nowMillis: Long): CaptureSummary {
        val frames = recorder.frames
        val sharpness = if (frames.isEmpty()) {
            0f
        } else {
            frames.map { it.quality.sharpness }.average().toFloat()
        }
        return CaptureSummary(
            frameCount = frameIndex,
            keyframeCount = frames.size,
            rejectedFrameCount = rejected,
            durationMillis = (nowMillis - startedAtMillis).coerceAtLeast(0),
            coverage = coverage.coverage,
            hasSensorDepth = frames.any { it.depthFileName != null },
            hasArPoses = frames.any { it.pose != null },
            sceneExtentMetres = sceneExtent(),
            meanSharpness = sharpness,
            predictedQuality = predictedQuality(sharpness),
        )
    }

    fun discard() = recorder.discard()

    // ---- internals ---------------------------------------------------------

    private fun shouldAttempt(inputs: GuidanceInputs, nowMillis: Long): Boolean {
        if (!GuidanceEngine.shouldRecordFrame(inputs)) return false
        if (recorder.frameCount >= budget.keyframeLimit) return false
        // The first keyframe is never rate-limited. Measuring the interval from a zero
        // initialiser would hold it back until the throttle period had elapsed, which means a
        // scan started and ended quickly would record nothing at all.
        if (recorder.frameCount == 0) return true
        val interval = budget.minIntervalMillis(conditions.thermalStatus)
        return nowMillis - lastKeyframeMillis >= interval
    }

    private fun computeSpeed(pose: Pose?, timestampNanos: Long): Float {
        val previous = lastPose
        val speed = if (previous != null && pose != null) {
            speedBetween(previous, pose, timestampNanos - lastPoseNanos)
        } else {
            0f
        }
        if (pose != null) {
            lastPose = pose
            lastPoseNanos = timestampNanos
        }
        return speed
    }

    private fun noteRejection(wasRejected: Boolean) {
        if (wasRejected) rejected++
        recentRejections.addLast(wasRejected)
        while (recentRejections.size > REJECTION_WINDOW) recentRejections.removeFirst()
    }

    private fun recentRejectionRate(): Float {
        if (recentRejections.isEmpty()) return 0f
        return recentRejections.count { it }.toFloat() / recentRejections.size
    }

    /**
     * Since the last frame was kept, measured from the start of recording while none has been.
     *
     * Measuring from the start is what makes "nothing is being kept" detectable at all: a
     * session that has never kept a frame has no last keyframe to measure from.
     */
    private fun millisSinceKeyframe(nowMillis: Long): Long {
        val since = if (lastKeyframeMillis > 0) lastKeyframeMillis else startedAtMillis
        return (nowMillis - since).coerceAtLeast(0)
    }

    private fun stats(
        nowMillis: Long,
        quality: FrameQuality,
        trackingState: TrackingState,
        speedMetresPerSecond: Float,
    ): CaptureStats {
        val elapsed = (nowMillis - startedAtMillis).coerceAtLeast(0)
        val rate = if (elapsed <= 0) {
            0f
        } else {
            recorder.frameCount * MILLIS_PER_SECOND / elapsed
        }
        return CaptureStats(
            frameCount = frameIndex,
            keyframeCount = recorder.frameCount,
            rejectedCount = rejected,
            coverage = coverage.coverage,
            predictedQuality = predictedQuality(quality.sharpness),
            storageUsedBytes = recorder.bytesWritten,
            storageAvailableBytes = conditions.storageAvailableBytes,
            elapsedMillis = elapsed,
            // The real state, not a constant. It was hard-coded to Tracking, which meant the
            // readout said tracking was fine while limited tracking was rejecting every frame
            // -- the single most useful fact about a scan that is producing nothing, hidden.
            trackingState = trackingState,
            thermalStatus = conditions.thermalStatus,
            batteryPercent = conditions.batteryPercent,
            captureRateHz = rate,
            speedMetresPerSecond = speedMetresPerSecond,
            millisSinceKeyframe = millisSinceKeyframe(nowMillis),
        )
    }

    /**
     * The extent of the scanned scene, used to size the TSDF volume and to estimate processing
     * time before the user commits to it.
     *
     * From the camera path's bounding box rather than from the geometry, because the geometry
     * does not exist yet. For an orbit that is twice the orbit radius, which is the right order
     * of magnitude for the subject inside it.
     */
    private fun sceneExtent(): Float {
        val poses = recorder.frames.mapNotNull { it.pose }
        if (poses.isEmpty()) return 0f
        var minX = Float.MAX_VALUE
        var minY = Float.MAX_VALUE
        var minZ = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE
        var maxY = -Float.MAX_VALUE
        var maxZ = -Float.MAX_VALUE
        for (pose in poses) {
            minX = minOf(minX, pose.translationX)
            minY = minOf(minY, pose.translationY)
            minZ = minOf(minZ, pose.translationZ)
            maxX = maxOf(maxX, pose.translationX)
            maxY = maxOf(maxY, pose.translationY)
            maxZ = maxOf(maxZ, pose.translationZ)
        }
        val extent = maxOf(maxX - minX, maxY - minY, maxZ - minZ)
        return extent.coerceAtLeast(MIN_SCENE_EXTENT_METRES)
    }

    /**
     * An estimate of how good the finished model will be, 0..1.
     *
     * Coverage dominates, because a hole in the model is what people actually notice; sharpness
     * and frame count modulate it. This is shown as a prediction, never as a measurement, and
     * it is deliberately conservative early in a scan so the number only ever improves.
     */
    private fun predictedQuality(sharpness: Float): Float {
        val coverageTerm = (coverage.coverage / CoverageTracker.SUFFICIENT_COVERAGE)
            .coerceIn(0f, 1f)
        val sharpnessTerm = (sharpness / GOOD_SHARPNESS).coerceIn(0f, 1f)
        val frameTerm = (recorder.frameCount.toFloat() / CaptureBudget.MIN_USEFUL_FRAMES)
            .coerceIn(0f, 1f)
        return (
            coverageTerm * COVERAGE_WEIGHT +
                sharpnessTerm * SHARPNESS_WEIGHT +
                frameTerm * FRAME_WEIGHT
            ).coerceIn(0f, 1f)
    }

    private companion object {
        const val REJECTION_WINDOW = 30
        const val MILLIS_PER_SECOND = 1_000f
        const val MIN_SCENE_EXTENT_METRES = 0.3f
        const val GOOD_SHARPNESS = 180f
        const val COVERAGE_WEIGHT = 0.6f
        const val SHARPNESS_WEIGHT = 0.25f
        const val FRAME_WEIGHT = 0.15f
    }
}

/** Frame scoring, behind an interface so the loop can be tested without the native library. */
interface FrameScorer {
    fun score(luma: ByteBuffer, width: Int, height: Int, rowStride: Int): FrameQuality
    fun hash(luma: ByteBuffer, width: Int, height: Int, rowStride: Int): Long
    fun distance(a: Long, b: Long): Int
}

/**
 * The real scorer: sharpness, exposure and contrast from the native engine.
 *
 * It runs on the luma plane directly out of the camera, with no copy and no decode, because it
 * runs on every preview frame and a multi-megapixel copy at 30 Hz would be the most expensive
 * thing in the app.
 */
object NativeFrameScorer : FrameScorer {

    override fun score(luma: ByteBuffer, width: Int, height: Int, rowStride: Int): FrameQuality {
        val values = ParallaxEngine.scoreFrame(luma, width, height, rowStride)
        if (values.size < SCORE_VALUES) return FrameQuality()
        return FrameQuality(
            sharpness = values[0].toFloat(),
            exposure = values[1].toFloat(),
            contrast = values[2].toFloat(),
            clippedLowFraction = values[3].toFloat(),
            clippedHighFraction = values[4].toFloat(),
            acceptable = values[5] > 0.5,
        )
    }

    override fun hash(luma: ByteBuffer, width: Int, height: Int, rowStride: Int): Long =
        ParallaxEngine.frameHash(luma, width, height, rowStride)

    override fun distance(a: Long, b: Long): Int = ParallaxEngine.hashDistance(a, b)

    private const val SCORE_VALUES = 6
}
