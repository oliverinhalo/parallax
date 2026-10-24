package uk.co.jacoblevy.parallax.feature.processing

import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.BoundingBox
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.CaptureSummary
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset

/**
 * The pipeline configuration and the JSON contract with the native bridge.
 *
 * The engine itself cannot run here -- there is no `.so` on a JVM host -- but the two things
 * most likely to break silently can be tested: that the configuration sent across is right for
 * the capture mode, and that the shapes the bridge emits deserialise. A mismatch in either
 * produces a reconstruction that is wrong rather than one that fails.
 */
@RunWith(RobolectricTestRunner::class)
class ReconstructionRunnerConfigTest {

    private lateinit var storage: ProjectStorage
    private lateinit var runner: ReconstructionRunner

    /**
     * The same settings the runner uses.
     *
     * `encodeDefaults` matters: with it off, a field left at its default is omitted and the C++
     * side silently falls back to its own default. Those two defaults agreeing today is luck,
     * not design, so the encoder sends every field.
     */
    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

    private val capableDevice = DeviceCapabilities(
        totalRamBytes = 8L * 1024 * 1024 * 1024,
        maxTextureSize = 8192,
        supportsDepthApi = true,
    )

    @Before
    fun setUp() {
        storage = ProjectStorage(ApplicationProvider.getApplicationContext())
        runner = ReconstructionRunner(storage, Dispatchers.Unconfined)
    }

    private fun project(
        mode: CaptureMode,
        quality: QualityPreset = QualityPreset.Balanced,
        hasPoses: Boolean = true,
        hasDepth: Boolean = true,
    ) = Project(
        id = "p1",
        name = "Scan",
        captureMode = mode,
        createdAtMillis = 0,
        updatedAtMillis = 0,
        status = ProjectStatus.Captured,
        quality = quality,
        capture = CaptureSummary(
            keyframeCount = 80,
            hasArPoses = hasPoses,
            hasSensorDepth = hasDepth,
            sceneExtentMetres = 1.5f,
        ),
    )

    /** The config is produced by `run`, so it is read back from the engine's rejection. */
    private fun configFor(
        project: Project,
        capabilities: DeviceCapabilities = capableDevice,
    ): PipelineConfigJson = runner.configForTesting(project, capabilities)

    @Test
    fun `object mode cuts the ground away and keeps only the subject`() {
        val config = configFor(project(CaptureMode.Object))

        assertThat(config.mode).isEqualTo("object")
        assertThat(config.removeGroundPlane).isTrue()
        assertThat(config.keepLargestComponent).isTrue()
        assertThat(config.analyseRoom).isFalse()
    }

    @Test
    fun `room mode keeps the floor and asks for a floor plan`() {
        val config = configFor(project(CaptureMode.Room))

        assertThat(config.mode).isEqualTo("room")
        // Removing the ground plane in a room would delete the floor, and then the room.
        assertThat(config.removeGroundPlane).isFalse()
        assertThat(config.keepLargestComponent).isFalse()
        assertThat(config.analyseRoom).isTrue()
    }

    @Test
    fun `the integration range grows with the size of the space`() {
        val objectRange = configFor(project(CaptureMode.Object)).maxIntegrationDepth
        val roomRange = configFor(project(CaptureMode.Room)).maxIntegrationDepth
        val areaRange = configFor(project(CaptureMode.Area)).maxIntegrationDepth

        // The voxel block budget is finite: integrating the floor stretching away behind a
        // small object spends it all on surface nobody wants, and the extraction comes back
        // empty. An area scan genuinely needs the range.
        assertThat(objectRange).isLessThan(roomRange)
        assertThat(roomRange).isLessThan(areaRange)
        assertThat(objectRange).isLessThan(2f)
    }

    @Test
    fun `the capture's own flags decide whether poses and depth are trusted`() {
        val withBoth = configFor(project(CaptureMode.Object, hasPoses = true, hasDepth = true))
        assertThat(withBoth.hasDevicePoses).isTrue()
        assertThat(withBoth.hasDepthMaps).isTrue()

        val imported = configFor(project(CaptureMode.Import, hasPoses = false, hasDepth = false))
        // Import must run full structure-from-motion, and the depth stage must estimate rather
        // than expect maps that do not exist.
        assertThat(imported.hasDevicePoses).isFalse()
        assertThat(imported.hasDepthMaps).isFalse()
    }

    @Test
    fun `the texture resolution is capped by what the GPU can hold`() {
        val modest = capableDevice.copy(maxTextureSize = 2048)
        val config = configFor(project(CaptureMode.Object, QualityPreset.Max), modest)

        assertThat(QualityPreset.Max.textureResolution).isGreaterThan(2048)
        assertThat(config.textureResolution).isEqualTo(2048)
    }

    @Test
    fun `the triangle budget is capped by what the atlas can texture`() {
        // Each triangle needs its own patch in the atlas. A target the atlas cannot hold
        // produces a mesh whose later triangles are simply untextured.
        val small = capableDevice.copy(maxTextureSize = 1024)
        val config = configFor(project(CaptureMode.Object, QualityPreset.Max), small)

        assertThat(config.targetTriangles).isLessThan(QualityPreset.Max.targetTriangles)
        assertThat(config.targetTriangles).isAtLeast(5_000)
    }

    @Test
    fun `the memory budget comes from the device, not from the preset`() {
        val small = capableDevice.copy(totalRamBytes = 2L * 1024 * 1024 * 1024)
        val large = capableDevice.copy(totalRamBytes = 16L * 1024 * 1024 * 1024)

        assertThat(configFor(project(CaptureMode.Room), small).memoryBudgetBytes)
            .isLessThan(configFor(project(CaptureMode.Room), large).memoryBudgetBytes)
    }

    @Test
    fun `the voxel size is the preset's`() {
        for (preset in QualityPreset.entries) {
            val config = configFor(project(CaptureMode.Object, preset))
            assertThat(config.voxelSize).isEqualTo(preset.voxelSizeMetres)
            assertThat(config.maxKeyframes).isEqualTo(preset.maxKeyframes)
        }
    }

    @Test
    fun `the project directory is absolute, because the engine resolves nothing`() {
        val config = configFor(project(CaptureMode.Object))
        assertThat(config.projectDir).startsWith("/")
        assertThat(config.projectDir).endsWith("p1")
    }

    // ---- the JSON the bridge emits -----------------------------------------

    @Test
    fun `a stage outcome deserialises, including the bounds array`() {
        // Exactly what ParallaxJni.cpp writes: bounds is a six-element array, not an object.
        val emitted = """
            {
              "success": true,
              "cancelled": false,
              "message": "model ready",
              "nextStage": 10,
              "complete": true,
              "summary": {
                "frameCount": 80,
                "keyframeCount": 60,
                "triangleCount": 120000,
                "vertexCount": 61000,
                "pointCount": 240000,
                "surfaceAreaM2": 1.25,
                "volumeM3": 0.031,
                "watertight": true,
                "hasTexture": true,
                "hasFloorPlan": false,
                "floorAreaM2": 0.0,
                "ceilingHeightM": 0.0,
                "scaleFactor": 1.0,
                "bounds": [-0.2, 0.0, -0.2, 0.2, 0.44, 0.2]
              }
            }
        """.trimIndent()

        val result = json.decodeFromString<StageResult>(emitted)

        assertThat(result.success).isTrue()
        assertThat(result.complete).isTrue()
        val summary = requireNotNull(result.summary)
        assertThat(summary.triangleCount).isEqualTo(120_000)
        assertThat(summary.bounds.toBoundingBox())
            .isEqualTo(BoundingBox(-0.2f, 0f, -0.2f, 0.2f, 0.44f, 0.2f))
    }

    @Test
    fun `a failure outcome deserialises with its message`() {
        val emitted = """
            {"success":false,"cancelled":false,"message":"no mesh to finalise","nextStage":9}
        """.trimIndent()

        val result = json.decodeFromString<StageResult>(emitted)

        assertThat(result.success).isFalse()
        assertThat(result.message).isEqualTo("no mesh to finalise")
        assertThat(result.summary).isNull()
    }

    @Test
    fun `a malformed bounds array degrades to an empty box rather than throwing`() {
        // The two sides have to agree exactly, and this is what happens if they ever stop:
        // an empty box, which the viewer reads as a model with no extent, rather than a crash.
        assertThat(listOf(1f, 2f).toBoundingBox()).isEqualTo(BoundingBox())
        assertThat(null.toBoundingBox()).isEqualTo(BoundingBox())
    }

    @Test
    fun `the config round trips through the serialiser the bridge reads`() {
        val config = configFor(project(CaptureMode.Room, QualityPreset.Max))
        val encoded = json.encodeToString<PipelineConfigJson>(config)

        // The key names are the contract with ParallaxJni.cpp, which parses them by hand.
        for (key in listOf(
            "projectDir", "mode", "voxelSize", "maxKeyframes", "textureResolution",
            "targetTriangles", "hasDevicePoses", "hasDepthMaps", "memoryBudgetBytes",
            "maxIntegrationDepth", "removeGroundPlane", "keepLargestComponent", "autoLevel",
            "buildPointCloud", "buildWatertight", "analyseRoom", "truncationMultiplier",
        )) {
            assertThat(encoded).contains("\"$key\"")
        }
    }

    @Test
    fun `the engine's absence is reported rather than crashing`() = runTest {
        // There is no native library on a JVM host, which is exactly what a bad install looks
        // like on a device: the run has to come back as a failure with a reason.
        val outcome = runner.run(
            project = project(CaptureMode.Object),
            capabilities = capableDevice,
            shouldCancel = { false },
            onProgress = {},
        )

        assertThat(outcome).isInstanceOf(ReconstructionOutcome.Failed::class.java)
        val failed = outcome as ReconstructionOutcome.Failed
        assertThat(failed.error.message).isNotEmpty()
        assertThat(failed.error.recovery).isNotNull()
    }
}
