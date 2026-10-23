package uk.co.jacoblevy.parallax.core.model

import kotlinx.serialization.Serializable

/**
 * A prompt shown during capture.
 *
 * Ordered by [priority] so that when several conditions are true at once -- which is the normal
 * case, since moving too fast also blurs frames -- the overlay shows the one that will actually
 * fix the scan rather than cycling through all of them.
 */
@Serializable
enum class GuidancePrompt(
    val message: String,
    val detail: String,
    val priority: Int,
    val haptic: HapticCue,
) {
    HoldStill(
        message = "Hold still",
        detail = "Tracking is recovering",
        priority = 100,
        haptic = HapticCue.Warning,
    ),
    SlowDown(
        message = "Slow down",
        detail = "Frames are blurring",
        priority = 90,
        haptic = HapticCue.Warning,
    ),
    MoreLight(
        message = "More light needed",
        detail = "Move somewhere brighter if you can",
        priority = 80,
        haptic = HapticCue.Warning,
    ),
    TooBright(
        message = "Too bright",
        detail = "Avoid pointing at the light",
        priority = 75,
        haptic = HapticCue.Warning,
    ),
    MoveCloser(
        message = "Move closer",
        detail = "Fill more of the frame with the subject",
        priority = 70,
        haptic = HapticCue.Hint,
    ),
    MoveBack(
        message = "Move back",
        detail = "You are too close to focus",
        priority = 70,
        haptic = HapticCue.Hint,
    ),
    CaptureLower(
        message = "Capture from lower down",
        detail = "The underside is still missing",
        priority = 60,
        haptic = HapticCue.Hint,
    ),
    CaptureHigher(
        message = "Capture from above",
        detail = "The top is still missing",
        priority = 60,
        haptic = HapticCue.Hint,
    ),
    MissedTheBack(
        message = "You missed the back",
        detail = "Walk around to the other side",
        priority = 55,
        haptic = HapticCue.Hint,
    ),
    LowTexture(
        message = "Surface is too plain",
        detail = "Blank walls are hard to track",
        priority = 50,
        haptic = HapticCue.Hint,
    ),
    Overheating(
        message = "Phone is getting warm",
        detail = "Capture rate reduced to cool down",
        priority = 95,
        haptic = HapticCue.Warning,
    ),
    LowBattery(
        message = "Battery is low",
        detail = "Finish up or plug in",
        priority = 85,
        haptic = HapticCue.Warning,
    ),
    /**
     * The highest priority of any prompt, above even lost tracking.
     *
     * Tracking recovers; storage does not. Telling someone to hold still while the last of
     * their free space fills up spends the only resource that cannot be got back from inside
     * the capture screen.
     */
    LowStorage(
        message = "Storage is nearly full",
        detail = "Free some space to keep scanning",
        priority = 110,
        haptic = HapticCue.Warning,
    ),
    KeepGoing(
        message = "Looking good",
        detail = "Keep moving around the subject",
        priority = 10,
        haptic = HapticCue.None,
    ),
    ReadyToFinish(
        message = "Ready to finish",
        detail = "Coverage is good; tap done when you are",
        priority = 20,
        haptic = HapticCue.Success,
    ),
}

@Serializable
enum class HapticCue { None, Hint, Warning, Success, Shutter }

/** Live capture statistics, driving the on-screen readout. */
@Serializable
data class CaptureStats(
    val frameCount: Int = 0,
    val keyframeCount: Int = 0,
    val rejectedCount: Int = 0,
    val coverage: Float = 0f,
    val predictedQuality: Float = 0f,
    val storageUsedBytes: Long = 0,
    val storageAvailableBytes: Long = 0,
    val elapsedMillis: Long = 0,
    val trackingState: TrackingState = TrackingState.Initialising,
    val thermalStatus: ThermalStatus = ThermalStatus.Nominal,
    val batteryPercent: Int = 100,
    val captureRateHz: Float = 0f,
) {
    val rejectionRate: Float
        get() = if (frameCount == 0) 0f else rejectedCount.toFloat() / frameCount
}

/**
 * Device thermal state, mapped from Android's PowerManager thermal status.
 *
 * Capture throttles progressively rather than stopping: a scan that dies halfway through is
 * worse than a scan with a slightly lower frame rate, and the user has often walked somewhere
 * specifically to take it.
 */
@Serializable
enum class ThermalStatus(val captureRateScale: Float, val shouldWarn: Boolean) {
    Nominal(1.0f, false),
    Fair(1.0f, false),
    Serious(0.6f, true),
    Critical(0.3f, true),
    /** Android is shutting things down; capture must stop and save what it has. */
    Emergency(0.0f, true),
    ;

    val mustStop: Boolean get() = this == Emergency
}

/** Coverage of the subject, as a sphere of bins around it. */
@Serializable
data class CoverageMap(
    val azimuthBins: Int = DEFAULT_AZIMUTH_BINS,
    val elevationBins: Int = DEFAULT_ELEVATION_BINS,
    /** Row-major [elevationBins] x [azimuthBins], each 0..1. */
    val bins: FloatArray = FloatArray(DEFAULT_AZIMUTH_BINS * DEFAULT_ELEVATION_BINS),
) {
    /**
     * Fraction of the sphere covered.
     *
     * A bin counts as covered once a quarter of it has been seen: demanding more would leave
     * the ring never reaching 100% on a subject that simply has no underside to photograph.
     */
    val coverage: Float
        get() = if (bins.isEmpty()) {
            0f
        } else {
            bins.count { it > COVERED_THRESHOLD }.toFloat() / bins.size
        }

    fun binAt(elevation: Int, azimuth: Int): Float {
        if (elevation < 0 || elevation >= elevationBins) return 0f
        if (azimuth < 0 || azimuth >= azimuthBins) return 0f
        return bins[elevation * azimuthBins + azimuth]
    }

    override fun equals(other: Any?): Boolean = this === other ||
        (
            other is CoverageMap && azimuthBins == other.azimuthBins &&
                elevationBins == other.elevationBins && bins.contentEquals(other.bins)
            )

    override fun hashCode(): Int =
        (azimuthBins * HASH_FACTOR + elevationBins) * HASH_FACTOR + bins.contentHashCode()

    companion object {
        /** 15 degrees of azimuth per bin, and 30 of elevation. */
        const val DEFAULT_AZIMUTH_BINS = 24
        const val DEFAULT_ELEVATION_BINS = 6
        const val COVERED_THRESHOLD = 0.25f
        private const val HASH_FACTOR = 31
    }
}
