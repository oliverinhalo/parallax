package uk.co.jacoblevy.parallax.core.data

import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.data.storage.CaptureManifest
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import java.io.File

@RunWith(RobolectricTestRunner::class)
class ProjectStorageTest {

    private lateinit var storage: ProjectStorage

    @Before
    fun setUp() {
        storage = ProjectStorage(ApplicationProvider.getApplicationContext())
    }

    @Test
    fun `create makes the directories the native pipeline expects`() {
        storage.create("p1")
        for (name in
            listOf(
                ProjectStorage.FRAMES,
                ProjectStorage.DEPTH,
                ProjectStorage.CHECKPOINTS,
                ProjectStorage.OUTPUTS,
                ProjectStorage.EXPORTS,
            )
        ) {
            assertThat(File(storage.projectDir("p1"), name).isDirectory).isTrue()
        }
    }

    @Test
    fun `create is idempotent so a resumed capture does not fail`() {
        storage.create("p1")
        storage.framesDir("p1").resolve("000001.jpg").writeBytes(ByteArray(16))
        storage.create("p1")
        assertThat(storage.framesDir("p1").listFiles()).hasLength(1)
    }

    @Test
    fun `manifest round trips through the exact wire names`() {
        val manifest = TestFixtures.manifest("p1")
        storage.writeManifest("p1", manifest)
        assertThat(storage.readManifest("p1")).isEqualTo(manifest)

        // The field names are a contract with Pipeline.cpp, which parses them by hand.
        val text = storage.manifestFile("p1").readText()
        for (key in listOf("\"frames\"", "\"image\"", "\"keyframe\"", "\"intrinsics\"", "\"fx\"")) {
            assertThat(text).contains(key)
        }
    }

    @Test
    fun `a manifest with no poses omits the pose field entirely`() {
        // The pipeline detects Import mode by the absence of the key, so an explicit null or an
        // identity matrix here would silently turn an import into a broken AR capture.
        storage.writeManifest("p1", TestFixtures.manifest("p1", withPoses = false))
        assertThat(storage.manifestFile("p1").readText()).doesNotContain("\"pose\"")
    }

    @Test
    fun `a corrupt manifest reads as null rather than throwing`() {
        storage.create("p1")
        storage.manifestFile("p1").writeText("{ truncated")
        assertThat(storage.readManifest("p1")).isNull()
    }

    @Test
    fun `purge removes the regenerable data and keeps the model`() {
        storage.create("p1")
        storage.framesDir("p1").resolve("000001.jpg").writeBytes(ByteArray(4_096))
        storage.depthDir("p1").resolve("000001.bin").writeBytes(ByteArray(2_048))
        storage.checkpointsDir("p1").resolve("mesh_raw.ply").writeBytes(ByteArray(1_024))
        storage.outputsDir("p1").resolve(ProjectStorage.MODEL_GLB).writeBytes(ByteArray(512))

        val freed = storage.purgeIntermediates("p1")

        assertThat(freed).isEqualTo(4_096 + 2_048 + 1_024)
        assertThat(storage.hasModel("p1")).isTrue()
        assertThat(storage.sizeOf("p1")).isEqualTo(512)
        // The directories survive, so resuming a capture into them does not need a re-create.
        assertThat(storage.framesDir("p1").isDirectory).isTrue()
    }

    @Test
    fun `size counts nested files`() {
        storage.create("p1")
        val nested = File(storage.outputsDir("p1"), "a/b/c").apply { mkdirs() }
        nested.resolve("deep.bin").writeBytes(ByteArray(777))
        assertThat(storage.sizeOf("p1")).isEqualTo(777)
    }

    @Test
    fun `usable capture needs a manifest and at least two frames`() {
        storage.create("p1")
        assertThat(storage.hasUsableCapture("p1")).isFalse()
        storage.writeManifest("p1", TestFixtures.manifest("p1"))
        assertThat(storage.hasUsableCapture("p1")).isFalse()
        storage.framesDir("p1").resolve("000001.jpg").writeBytes(ByteArray(1))
        assertThat(storage.hasUsableCapture("p1")).isFalse()
        storage.framesDir("p1").resolve("000002.jpg").writeBytes(ByteArray(1))
        assertThat(storage.hasUsableCapture("p1")).isTrue()
    }

    @Test
    fun `orphaned directories are reclaimed`() {
        storage.create("kept")
        storage.create("orphan")
        storage.outputsDir("orphan").resolve("model.glb").writeBytes(ByteArray(2_000))

        val freed = storage.deleteOrphans(setOf("kept"))

        assertThat(freed).isEqualTo(2_000)
        assertThat(storage.exists("orphan")).isFalse()
        assertThat(storage.exists("kept")).isTrue()
    }

    @Test
    fun `breakdown attributes each file to the right category`() {
        storage.create("p1")
        storage.framesDir("p1").resolve("000001.jpg").writeBytes(ByteArray(1_000))
        storage.depthDir("p1").resolve("000001.bin").writeBytes(ByteArray(100))
        storage.checkpointsDir("p1").resolve("poses.json").writeBytes(ByteArray(10))
        storage.outputsDir("p1").resolve("model.glb").writeBytes(ByteArray(2_000))
        storage.outputsDir("p1").resolve("cloud.ply").writeBytes(ByteArray(3_000))
        storage.outputsDir("p1").resolve("splat.spz").writeBytes(ByteArray(4_000))
        storage.outputsDir("p1").resolve("floorplan.svg").writeBytes(ByteArray(50))
        storage.thumbnailFile("p1").writeBytes(ByteArray(7))

        val breakdown = storage.breakdown()

        assertThat(breakdown.rawFramesBytes).isEqualTo(1_000)
        assertThat(breakdown.depthBytes).isEqualTo(100)
        assertThat(breakdown.checkpointBytes).isEqualTo(10)
        assertThat(breakdown.meshBytes).isEqualTo(2_000)
        assertThat(breakdown.pointCloudBytes).isEqualTo(3_000)
        assertThat(breakdown.splatBytes).isEqualTo(4_000)
        assertThat(breakdown.textureBytes).isEqualTo(50)
        assertThat(breakdown.thumbnailBytes).isEqualTo(7)
        assertThat(breakdown.reclaimableBytes).isEqualTo(1_000 + 100 + 10)
    }

    @Test
    fun `manifest version is recorded so a future format change is detectable`() {
        storage.writeManifest("p1", TestFixtures.manifest("p1"))
        assertThat(storage.readManifest("p1")?.version).isEqualTo(CaptureManifest.VERSION)
        assertThat(storage.readManifest("p1")?.mode).isEqualTo(CaptureMode.Object.name)
    }
}
