package uk.co.jacoblevy.parallax.feature.capture.session

import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.CameraIntrinsics
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.GuidancePrompt
import uk.co.jacoblevy.parallax.core.model.Pose
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ThermalStatus
import uk.co.jacoblevy.parallax.core.model.TrackingState
import uk.co.jacoblevy.parallax.feature.capture.logic.CaptureBudget
import java.nio.ByteBuffer
import kotlin.math.cos
import kotlin.math.sin

/**
 * Drives the capture loop with a scripted scorer and a recorder that writes nothing.
 *
 * The camera and the filesystem are the two things that cannot run here, so both are behind
 * seams: a [FrameScorer] the test scripts, and a [FrameRecorder] subclass that counts calls
 * instead of encoding JPEG. Everything between them -- rate limiting, keyframe selection,
 * coverage, guidance, the summary -- is the part worth testing, and it all runs.
 */
@RunWith(RobolectricTestRunner::class)
class CaptureEngineTest {

    private lateinit var storage: ProjectStorage
    private lateinit var recorder: CountingRecorder

    private var scriptedQuality = FrameQuality(
        sharpness = 150f,
        exposure = 120f,
        contrast = 40f,
        acceptable = true,
    )
    private var scriptedHash = 0L
    private var scriptedDistance = 64

    private val scorer = object : FrameScorer {
        override fun score(
            luma: ByteBuffer,
            width: Int,
            height: Int,
            rowStride: Int,
        ): FrameQuality = scriptedQuality

        override fun hash(luma: ByteBuffer, width: Int, height: Int, rowStride: Int): Long =
            scriptedHash

        override fun distance(a: Long, b: Long): Int = scriptedDistance
    }

    /** A recorder that records the metadata and skips the image encoding. */
    private class CountingRecorder(storage: ProjectStorage, projectId: String) :
        FrameRecorder(storage, projectId) {
        var recordCalls = 0
            private set

        override fun record(
            index: Int,
            image: android.media.Image?,
            depth: android.media.Image?,
            pose: Pose?,
            intrinsics: CameraIntrinsics,
            quality: FrameQuality,
            imageHash: Long,
            isKeyframe: Boolean,
            trackingState: TrackingState,
            timestampNanos: Long,
        ): uk.co.jacoblevy.parallax.core.model.CaptureFrame? {
            recordCalls++
            return recordMetadata(
                index = index,
                imageName = "%06d.jpg".format(index),
                depthName = null,
                pose = pose,
                intrinsics = intrinsics,
                quality = quality,
                imageHash = imageHash,
                isKeyframe = isKeyframe,
                trackingState = trackingState,
                timestampNanos = timestampNanos,
                bytes = SYNTHETIC_FRAME_BYTES,
            )
        }

        private companion object {
            const val SYNTHETIC_FRAME_BYTES = 1_024L
        }
    }

    private val intrinsics = CameraIntrinsics(
        focalLengthX = 1_440f,
        focalLengthY = 1_440f,
        principalPointX = 960f,
        principalPointY = 540f,
        imageWidth = 1_920,
        imageHeight = 1_080,
    )

    private val luma: ByteBuffer = ByteBuffer.allocateDirect(16 * 16)

    @Before
    fun setUp() {
        storage = ProjectStorage(ApplicationProvider.getApplicationContext())
        recorder = CountingRecorder(storage, "p1")
    }

    private fun engine(
        mode: CaptureMode = CaptureMode.Object,
        preset: QualityPreset = QualityPreset.Balanced,
        capabilities: DeviceCapabilities = DeviceCapabilities(
            totalRamBytes = 8L * 1024 * 1024 * 1024,
        ),
    ) = CaptureEngine(
        mode = mode,
        budget = CaptureBudget(capabilities, mode, preset),
        recorder = recorder,
        scorer = scorer,
    )

    private fun poseAt(x: Float, y: Float = 0f, z: Float = 0f) = Pose(
        floatArrayOf(
            1f, 0f, 0f, 0f,
            0f, 1f, 0f, 0f,
            0f, 0f, 1f, 0f,
            x, y, z, 1f,
        ),
    )

    private fun CaptureEngine.feed(
        nowMillis: Long,
        pose: Pose?,
        tracking: TrackingState = TrackingState.Tracking,
        haveImage: Boolean = true,
    ): CaptureTick = onFrame(
        nowMillis = nowMillis,
        timestampNanos = nowMillis * 1_000_000,
        trackingState = tracking,
        pose = pose,
        intrinsics = intrinsics,
        luma = luma,
        lumaWidth = 16,
        lumaHeight = 16,
        lumaRowStride = 16,
        image = if (haveImage) SyntheticImage else null,
        depth = null,
    )

    @Test
    fun `the first frame of a tracking session is kept`() {
        val engine = engine()
        engine.start(nowMillis = 0)
        engine.updateConditions(conditions())

        val tick = engine.feed(nowMillis = 0, pose = poseAt(0f))

        assertThat(tick.recorded).isNotNull()
        assertThat(engine.keyframeCount).isEqualTo(1)
    }

    @Test
    fun `nothing is recorded while tracking is lost`() {
        val engine = engine()
        engine.start(0)
        engine.updateConditions(conditions())

        val tick = engine.feed(0, pose = null, tracking = TrackingState.Lost)

        assertThat(tick.recorded).isNull()
        assertThat(tick.prompt).isEqualTo(GuidancePrompt.HoldStill)
        assertThat(engine.keyframeCount).isEqualTo(0)
    }

    @Test
    fun `a blurred frame is rejected and the advice says why`() {
        val engine = engine()
        engine.start(0)
        engine.updateConditions(conditions())
        scriptedQuality = FrameQuality(sharpness = 4f, exposure = 120f, contrast = 40f)

        val tick = engine.feed(0, pose = poseAt(0f))

        assertThat(tick.recorded).isNull()
        assertThat(tick.prompt).isEqualTo(GuidancePrompt.SlowDown)
        assertThat(tick.stats.rejectedCount).isEqualTo(1)
    }

    @Test
    fun `the capture rate is limited, so a 30Hz camera does not become 30 keyframes a second`() {
        val engine = engine()
        engine.start(0)
        engine.updateConditions(conditions())

        // Object mode targets four keyframes a second, so a 33 ms camera cadence has to be
        // throttled or a ten-second scan would produce 300 frames.
        var now = 0L
        repeat(60) {
            engine.feed(now, pose = poseAt(now / 100f))
            now += 33
        }

        assertThat(engine.keyframeCount).isAtMost(12)
        assertThat(engine.keyframeCount).isAtLeast(6)
    }

    @Test
    fun `a hot device captures more slowly than a cool one`() {
        val cool = engine()
        cool.start(0)
        cool.updateConditions(conditions(ThermalStatus.Nominal))
        var now = 0L
        repeat(40) {
            cool.feed(now, pose = poseAt(now / 50f))
            now += 50
        }

        recorder = CountingRecorder(storage, "p2")
        val hot = engine()
        hot.start(0)
        hot.updateConditions(conditions(ThermalStatus.Critical))
        now = 0L
        repeat(40) {
            hot.feed(now, pose = poseAt(now / 50f))
            now += 50
        }

        assertThat(hot.keyframeCount).isLessThan(cool.keyframeCount)
    }

    @Test
    fun `an overheating device stops recording entirely at emergency`() {
        val engine = engine()
        engine.start(0)
        engine.updateConditions(conditions(ThermalStatus.Emergency))

        val tick = engine.feed(0, pose = poseAt(0f))

        assertThat(tick.recorded).isNull()
    }

    @Test
    fun `a nearly full disk stops recording before it corrupts what it has`() {
        val engine = engine()
        engine.start(0)
        engine.updateConditions(conditions(storageBytes = 1_024))

        val tick = engine.feed(0, pose = poseAt(0f))

        assertThat(tick.recorded).isNull()
        assertThat(tick.prompt).isEqualTo(GuidancePrompt.LowStorage)
    }

    @Test
    fun `the keyframe limit is respected`() {
        val engine = engine(preset = QualityPreset.Fast)
        engine.start(0)
        engine.updateConditions(conditions())

        var now = 0L
        repeat(2_000) {
            engine.feed(now, pose = poseAt(now / 10f))
            now += 300
        }

        assertThat(engine.keyframeCount).isAtMost(QualityPreset.Fast.maxKeyframes)
    }

    @Test
    fun `coverage grows as the user orbits, and feeds the summary`() {
        val engine = engine()
        engine.start(0)
        engine.updateConditions(conditions())
        engine.setSubjectCentre(0f, 0f, 0f)

        var now = 0L
        for (degrees in 0 until 360 step 6) {
            val radians = Math.toRadians(degrees.toDouble())
            engine.feed(
                now,
                pose = poseAt(cos(radians).toFloat(), 0f, sin(radians).toFloat()),
            )
            now += 260
        }

        val summary = engine.summarise(now)
        assertThat(summary.coverage).isGreaterThan(0f)
        assertThat(summary.keyframeCount).isEqualTo(engine.keyframeCount)
        assertThat(summary.hasArPoses).isTrue()
        assertThat(summary.meanSharpness).isWithin(1f).of(150f)
        assertThat(summary.sceneExtentMetres).isGreaterThan(1f)
    }

    @Test
    fun `the predicted quality only ever improves during a scan`() {
        val engine = engine()
        engine.start(0)
        engine.updateConditions(conditions())
        engine.setSubjectCentre(0f, 0f, 0f)

        var now = 0L
        var previous = 0f
        for (degrees in 0 until 720 step 6) {
            val radians = Math.toRadians(degrees.toDouble())
            val tick = engine.feed(
                now,
                pose = poseAt(cos(radians).toFloat(), 0f, sin(radians).toFloat()),
            )
            // A prediction that goes down mid-scan reads as the app changing its mind, so it is
            // built only from quantities that grow.
            assertThat(tick.stats.predictedQuality).isAtLeast(previous - 1e-4f)
            previous = tick.stats.predictedQuality
            now += 260
        }
        assertThat(previous).isGreaterThan(0.5f)
    }

    @Test
    fun `a summary of an empty capture is zeroes rather than a crash`() {
        val engine = engine()
        engine.start(0)
        val summary = engine.summarise(nowMillis = 5_000)

        assertThat(summary.keyframeCount).isEqualTo(0)
        assertThat(summary.meanSharpness).isEqualTo(0f)
        assertThat(summary.sceneExtentMetres).isEqualTo(0f)
        assertThat(summary.durationMillis).isEqualTo(5_000)
    }

    @Test
    fun `an import-style feed with no poses still selects frames by novelty`() {
        val engine = engine(mode = CaptureMode.Import)
        engine.start(0)
        engine.updateConditions(conditions())

        scriptedDistance = 40
        repeat(5) { engine.feed(it * 10L, pose = null) }
        val afterNovelFrames = engine.keyframeCount

        scriptedDistance = 1
        repeat(5) { engine.feed(100L + it * 10L, pose = null) }

        assertThat(afterNovelFrames).isAtLeast(2)
        // Near-duplicates add nothing, which is the whole point of hashing the image.
        assertThat(engine.keyframeCount).isEqualTo(afterNovelFrames)
    }

    @Test
    fun `discarding a capture removes the frames it wrote`() {
        val engine = engine()
        engine.start(0)
        engine.updateConditions(conditions())
        engine.feed(0, pose = poseAt(0f))

        engine.discard()

        assertThat(engine.frames).isEmpty()
        assertThat(storage.framesDir("p1").listFiles()).isEmpty()
    }

    @Test
    fun `restarting clears the previous session's state`() {
        val engine = engine()
        engine.start(0)
        engine.updateConditions(conditions())
        engine.setSubjectCentre(0f, 0f, 0f)
        engine.feed(0, pose = poseAt(1f))
        assertThat(engine.keyframeCount).isEqualTo(1)

        engine.start(nowMillis = 10_000)

        assertThat(engine.keyframeCount).isEqualTo(0)
        assertThat(engine.summarise(10_000).coverage).isEqualTo(0f)
    }

    private fun conditions(
        thermal: ThermalStatus = ThermalStatus.Nominal,
        storageBytes: Long = 20L * 1024 * 1024 * 1024,
        battery: Int = 80,
    ) = DeviceConditions(
        thermalStatus = thermal,
        batteryPercent = battery,
        charging = false,
        storageAvailableBytes = storageBytes,
    )

    /**
     * A stand-in for a camera image.
     *
     * The recorder under test never touches it, and `android.media.Image` has no public
     * constructor, so this is the one place the test asserts the seam rather than the behaviour.
     */
    private companion object {
        val SyntheticImage: android.media.Image? = null
    }
}
