package uk.co.jacoblevy.parallax.feature.capture.logic

import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.FrameRejection
import uk.co.jacoblevy.parallax.core.model.GuidancePrompt
import uk.co.jacoblevy.parallax.core.model.ThermalStatus
import uk.co.jacoblevy.parallax.core.model.TrackingState

/** Everything the guidance engine looks at. One value object so the decision is pure. */
data class GuidanceInputs(
    val mode: CaptureMode,
    val trackingState: TrackingState = TrackingState.Tracking,
    val quality: FrameQuality = FrameQuality(acceptable = true),
    val coverage: Float = 0f,
    val coverageGap: CoverageGap = CoverageGap.None,
    /** Metres per second, from the camera path. */
    val speedMetresPerSecond: Float = 0f,
    /** Distance to what is being scanned, in metres, or null when it is not known. */
    val subjectDistanceMetres: Float? = null,
    val thermalStatus: ThermalStatus = ThermalStatus.Nominal,
    val batteryPercent: Int = 100,
    val storageAvailableBytes: Long = Long.MAX_VALUE,
    val keyframeCount: Int = 0,
    /** Fraction of recent frames that were rejected. */
    val recentRejectionRate: Float = 0f,
)

/**
 * Decides what to tell the user, right now.
 *
 * Deliberately a pure function of [GuidanceInputs]: during capture several conditions are true
 * at once -- moving too fast also blurs frames, which also means the frame is rejected, which
 * also means coverage is not growing -- and showing all of them would be noise. Each condition
 * maps to a [GuidancePrompt] with a priority, and the highest-priority one wins.
 *
 * The hysteresis in [GuidanceSession] is what makes this usable in practice. The decision here
 * is instantaneous; a prompt that appears and vanishes within three frames is worse than no
 * prompt at all.
 */
object GuidanceEngine {

    fun evaluate(inputs: GuidanceInputs): GuidancePrompt? {
        val candidates = buildList {
            addAll(sessionEndingPrompts(inputs))
            addAll(frameQualityPrompts(inputs))
            addAll(framingPrompts(inputs))
            addAll(coveragePrompts(inputs))
        }
        return candidates.maxByOrNull { it.priority }
    }

    /** Conditions that will end the scan if ignored. */
    private fun sessionEndingPrompts(inputs: GuidanceInputs): List<GuidancePrompt> = buildList {
        if (inputs.storageAvailableBytes < LOW_STORAGE_BYTES) add(GuidancePrompt.LowStorage)
        if (inputs.thermalStatus.shouldWarn) add(GuidancePrompt.Overheating)
        // A reading of zero means "not known yet" on some devices, and a battery warning on a
        // fully charged phone destroys trust in every other prompt.
        if (inputs.batteryPercent in 1..LOW_BATTERY_PERCENT) add(GuidancePrompt.LowBattery)
    }

    /** Conditions that make the frames being captured right now useless. */
    private fun frameQualityPrompts(inputs: GuidanceInputs): List<GuidancePrompt> = buildList {
        when (inputs.trackingState) {
            TrackingState.Limited, TrackingState.Lost, TrackingState.Initialising ->
                add(GuidancePrompt.HoldStill)
            TrackingState.Tracking -> Unit
        }
        if (inputs.speedMetresPerSecond > MAX_SPEED_METRES_PER_SECOND) {
            add(GuidancePrompt.SlowDown)
        }
        when (inputs.quality.rejectionReason) {
            FrameRejection.Blurred -> add(GuidancePrompt.SlowDown)
            FrameRejection.TooDark, FrameRejection.Underexposed -> add(GuidancePrompt.MoreLight)
            FrameRejection.TooBright, FrameRejection.Overexposed -> add(GuidancePrompt.TooBright)
            FrameRejection.LowTexture -> add(GuidancePrompt.LowTexture)
            FrameRejection.Duplicate, FrameRejection.NoPose, FrameRejection.Unknown, null -> Unit
        }
        // A high rejection rate with no single cause still needs saying: something is wrong
        // with how the phone is being moved even if this frame happened to pass.
        if (inputs.recentRejectionRate > HIGH_REJECTION_RATE &&
            inputs.quality.rejectionReason == null
        ) {
            add(GuidancePrompt.SlowDown)
        }
    }

    /** Framing, which only applies when there is something to frame. */
    private fun framingPrompts(inputs: GuidanceInputs): List<GuidancePrompt> = buildList {
        val distance = inputs.subjectDistanceMetres ?: return@buildList
        if (inputs.mode == CaptureMode.Object && distance > MAX_OBJECT_DISTANCE_METRES) {
            add(GuidancePrompt.MoveCloser)
        }
        // Below the lens's close-focus limit the frames are soft whatever else is true, in
        // every mode.
        if (distance < MIN_FOCUS_DISTANCE_METRES) add(GuidancePrompt.MoveBack)
    }

    /** What is still missing, once there is enough of a scan for gaps to mean anything. */
    private fun coveragePrompts(inputs: GuidanceInputs): List<GuidancePrompt> = buildList {
        if (inputs.keyframeCount >= MIN_FRAMES_FOR_COVERAGE_ADVICE) {
            when (inputs.coverageGap) {
                CoverageGap.Underside -> add(GuidancePrompt.CaptureLower)
                CoverageGap.Top -> add(GuidancePrompt.CaptureHigher)
                CoverageGap.Back -> add(GuidancePrompt.MissedTheBack)
                CoverageGap.Sides -> add(GuidancePrompt.KeepGoing)
                CoverageGap.None -> Unit
            }
        }
        // The encouraging cases, which lose to everything above.
        if (inputs.coverage >= CoverageTracker.SUFFICIENT_COVERAGE) {
            add(GuidancePrompt.ReadyToFinish)
        } else if (inputs.keyframeCount > 0) {
            add(GuidancePrompt.KeepGoing)
        }
    }

    /** A frame is worth keeping only while tracking and quality both hold. */
    fun shouldRecordFrame(inputs: GuidanceInputs): Boolean =
        inputs.trackingState == TrackingState.Tracking &&
            inputs.quality.acceptable &&
            !inputs.thermalStatus.mustStop &&
            inputs.storageAvailableBytes >= MIN_STORAGE_BYTES

    /** Below this the scan has to stop before it corrupts what it already has. */
    const val MIN_STORAGE_BYTES = 150L * 1024 * 1024
    const val LOW_STORAGE_BYTES = 600L * 1024 * 1024
    const val LOW_BATTERY_PERCENT = 15
    const val MAX_SPEED_METRES_PER_SECOND = 0.55f
    const val MAX_OBJECT_DISTANCE_METRES = 2.2f

    /** Below this most phone cameras cannot focus, so the frames are soft whatever else is true. */
    const val MIN_FOCUS_DISTANCE_METRES = 0.18f
    const val HIGH_REJECTION_RATE = 0.4f
    const val MIN_FRAMES_FOR_COVERAGE_ADVICE = 12
}

/**
 * Adds hysteresis to [GuidanceEngine], so prompts are readable.
 *
 * Two rules, both learned from how unusable the raw decision is: a prompt stays for at least
 * [GuidancePrompt.PROMPT_MIN_VISIBLE_MILLIS] once shown, and a *lower*-priority prompt cannot
 * replace it until that time has passed. A higher-priority one replaces it immediately, because
 * "storage nearly full" must not wait behind "looking good".
 */
class GuidanceSession(private val minVisibleMillis: Long = DEFAULT_MIN_VISIBLE_MILLIS) {

    private var current: GuidancePrompt? = null
    private var shownAtMillis = 0L

    val prompt: GuidancePrompt? get() = current

    /** Returns true when the visible prompt changed, which is when a haptic should fire. */
    fun update(inputs: GuidanceInputs, nowMillis: Long): Boolean {
        val next = GuidanceEngine.evaluate(inputs)
        val visible = current
        if (next == visible) return false

        val held = nowMillis - shownAtMillis
        val higherPriority = next != null && (visible == null || next.priority > visible.priority)
        if (visible != null && held < minVisibleMillis && !higherPriority) return false

        current = next
        shownAtMillis = nowMillis
        return true
    }

    fun reset() {
        current = null
        shownAtMillis = 0L
    }

    companion object {
        const val DEFAULT_MIN_VISIBLE_MILLIS = 1_400L
    }
}
