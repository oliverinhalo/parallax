package uk.co.jacoblevy.parallax.feature.capture.logic

import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ThermalStatus

/**
 * How fast to capture, and when to stop.
 *
 * Capture is the one part of the app that competes with the camera pipeline, ARCore's tracking
 * and the display for the same thermal headroom. The policy here is to degrade gradually: a scan
 * at a lower frame rate is still a scan, while one that trips the system's thermal limits
 * mid-session loses everything the user walked around for.
 */
class CaptureBudget(
    private val capabilities: DeviceCapabilities,
    private val mode: CaptureMode,
    private val preset: QualityPreset,
) {
    /**
     * Target keyframes per second.
     *
     * The base rate is per mode, not per device: it is set by how fast a person moves, which a
     * faster phone does not change. Thermal state then scales it.
     */
    fun targetKeyframesPerSecond(thermal: ThermalStatus): Float {
        val base = when (mode) {
            CaptureMode.Object -> OBJECT_KEYFRAMES_PER_SECOND
            CaptureMode.Room -> ROOM_KEYFRAMES_PER_SECOND
            CaptureMode.Area -> AREA_KEYFRAMES_PER_SECOND
            CaptureMode.Import -> UNTHROTTLED
        }
        return base * thermal.captureRateScale
    }

    /** Minimum interval between keyframes, in milliseconds. Zero means unthrottled. */
    fun minIntervalMillis(thermal: ThermalStatus): Long {
        val rate = targetKeyframesPerSecond(thermal)
        return if (rate <= 0f) 0L else (MILLIS_PER_SECOND / rate).toLong()
    }

    /** The keyframe ceiling. Beyond this the pipeline would decimate anyway. */
    val keyframeLimit: Int
        get() {
            val byPreset = preset.maxKeyframes
            // Area scans are chunked, so they are allowed more frames than one TSDF volume
            // would hold.
            val byMode = if (mode == CaptureMode.Area) byPreset * AREA_MULTIPLIER else byPreset
            return minOf(byMode, byRam())
        }

    /**
     * Keyframes the device has the memory to process.
     *
     * Derived from the reconstruction budget rather than from free RAM at capture time: the
     * limit that matters is whether the *processing* will fit, and that happens later, possibly
     * after a reboot.
     */
    private fun byRam(): Int {
        val budget = capabilities.reconstructionMemoryBudgetBytes
        return (budget / BYTES_PER_KEYFRAME_IN_MEMORY).toInt().coerceIn(MIN_KEYFRAMES, MAX_KEYFRAMES)
    }

    /** Bytes a capture of [keyframeCount] frames will occupy on disk. */
    fun projectedBytes(keyframeCount: Int): Long =
        keyframeCount.toLong() * bytesPerKeyframeOnDisk()

    /** How many more keyframes will fit in [availableBytes], leaving a working margin. */
    fun remainingCapacity(availableBytes: Long): Int {
        val usable = availableBytes - GuidanceEngine.MIN_STORAGE_BYTES
        if (usable <= 0) return 0
        return (usable / bytesPerKeyframeOnDisk()).toInt()
    }

    /** True once the scan has what it needs and more frames would only add cost. */
    fun hasEnoughFrames(keyframeCount: Int, coverage: Float): Boolean =
        keyframeCount >= keyframeLimit ||
            (coverage >= CoverageTracker.SUFFICIENT_COVERAGE && keyframeCount >= MIN_USEFUL_FRAMES)

    private fun bytesPerKeyframeOnDisk(): Long {
        // A JPEG at capture resolution, plus a 16-bit depth map when the device has one.
        val image = JPEG_BYTES
        val depth = if (capabilities.supportsDepthApi) DEPTH_BYTES else 0L
        return image + depth
    }

    companion object {
        const val MIN_USEFUL_FRAMES = 20

        /** An orbit of a small object covers a lot of angle per second. */
        private const val OBJECT_KEYFRAMES_PER_SECOND = 4f

        /** Walking through a room is slower, and the frames are further apart. */
        private const val ROOM_KEYFRAMES_PER_SECOND = 3f
        private const val AREA_KEYFRAMES_PER_SECOND = 2.5f

        /** Import reads from disk as fast as it can; a rate limit is meaningless. */
        private const val UNTHROTTLED = 0f
        private const val MILLIS_PER_SECOND = 1_000f
        private const val AREA_MULTIPLIER = 2
        private const val MIN_KEYFRAMES = 40
        private const val MAX_KEYFRAMES = 600

        /**
         * Peak in-memory cost per keyframe during reconstruction: the decoded image, its depth
         * map, its features and its descriptors. Measured across the synthetic test pipeline.
         */
        private const val BYTES_PER_KEYFRAME_IN_MEMORY = 6L * 1024 * 1024

        private const val JPEG_BYTES = 1_600L * 1024
        private const val DEPTH_BYTES = 320L * 240 * 2
    }
}
