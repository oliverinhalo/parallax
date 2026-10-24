package uk.co.jacoblevy.parallax.core.data

import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.ParallaxException
import uk.co.jacoblevy.parallax.core.data.storage.BundleMetadata
import uk.co.jacoblevy.parallax.core.data.storage.ProjectBundle
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.Annotation
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.Point3
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

@RunWith(RobolectricTestRunner::class)
class ProjectBundleTest {

    private lateinit var storage: ProjectStorage
    private lateinit var bundles: ProjectBundle

    private val project = Project(
        id = "source",
        name = "Garden bench",
        captureMode = CaptureMode.Object,
        createdAtMillis = 1_000,
        updatedAtMillis = 2_000,
        status = ProjectStatus.Ready(),
        outputs = setOf(OutputKind.Mesh, OutputKind.TexturedMesh),
    )

    private val annotations = listOf(
        Annotation("a1", "Crack", "near the left leg", Point3(0.1f, 0.2f, 0.3f)),
    )
    private val measurements = listOf<Measurement>(
        Measurement.Distance("m1", "Width", 0f, 0f, 0f, 1.4f, 0f, 0f),
    )

    @Before
    fun setUp() {
        storage = ProjectStorage(ApplicationProvider.getApplicationContext())
        bundles = ProjectBundle(storage)
        storage.create("source")
        storage.writeManifest("source", TestFixtures.manifest("source"))
        storage.outputsDir("source").resolve(ProjectStorage.MODEL_GLB).writeBytes(ByteArray(900))
        storage.outputsDir("source").resolve("atlas.png").writeBytes(ByteArray(300))
        storage.framesDir("source").resolve("000000.jpg").writeBytes(ByteArray(5_000))
        storage.thumbnailFile("source").writeBytes(ByteArray(40))
    }

    private fun export(includeRawFrames: Boolean): ByteArray {
        val out = ByteArrayOutputStream()
        bundles.write(out, project, annotations, measurements, includeRawFrames, nowMillis = 9)
        return out.toByteArray()
    }

    @Test
    fun `a bundle round trips as a new project`() {
        val bytes = export(includeRawFrames = false)
        val metadata = bundles.read(ByteArrayInputStream(bytes), "imported", nowMillis = 50)

        assertThat(metadata.project.id).isEqualTo("imported")
        assertThat(metadata.project.name).isEqualTo("Garden bench")
        assertThat(metadata.project.updatedAtMillis).isEqualTo(50)
        assertThat(metadata.annotations).isEqualTo(annotations)
        assertThat(metadata.measurements).isEqualTo(measurements)

        assertThat(storage.hasModel("imported")).isTrue()
        assertThat(storage.outputsDir("imported").resolve("atlas.png").length()).isEqualTo(300)
        assertThat(storage.readManifest("imported")).isEqualTo(storage.readManifest("source"))
        assertThat(storage.thumbnailFile("imported").length()).isEqualTo(40)
        // Importing must never touch the project it was exported from.
        assertThat(storage.hasModel("source")).isTrue()
    }

    @Test
    fun `raw frames are excluded by default and included on request`() {
        bundles.read(ByteArrayInputStream(export(includeRawFrames = false)), "lean", nowMillis = 1)
        assertThat(storage.framesDir("lean").listFiles()).isEmpty()

        bundles.read(ByteArrayInputStream(export(includeRawFrames = true)), "full", nowMillis = 1)
        assertThat(storage.framesDir("full").resolve("000000.jpg").length()).isEqualTo(5_000)
    }

    @Test
    fun `storage bytes are recomputed from what actually arrived`() {
        val metadata =
            bundles.read(ByteArrayInputStream(export(includeRawFrames = false)), "i", nowMillis = 1)
        assertThat(metadata.project.storageBytes).isEqualTo(storage.sizeOf("i"))
        assertThat(metadata.project.storageBytes).isGreaterThan(0L)
    }

    @Test
    fun `an entry escaping the project directory is refused`() {
        val malicious = ByteArrayOutputStream().also { out ->
            ZipOutputStream(out).use { zip ->
                zip.putNextEntry(ZipEntry(ProjectBundle.METADATA))
                zip.write(validMetadataJson().toByteArray())
                zip.closeEntry()
                zip.putNextEntry(ZipEntry("../../../databases/parallax.db"))
                zip.write(ByteArray(8))
                zip.closeEntry()
            }
        }.toByteArray()

        val failure = runCatching {
            bundles.read(ByteArrayInputStream(malicious), "evil", nowMillis = 1)
        }.exceptionOrNull()

        assertThat(failure).isInstanceOf(ParallaxException::class.java)
        assertThat((failure as ParallaxException).error)
            .isInstanceOf(ParallaxError.BundleInvalid::class.java)
        // And nothing is left behind for the library to show.
        assertThat(storage.exists("evil")).isFalse()
    }

    @Test
    fun `a bundle with no metadata entry is refused`() {
        val noMetadata = ByteArrayOutputStream().also { out ->
            ZipOutputStream(out).use { zip ->
                zip.putNextEntry(ZipEntry("outputs/model.glb"))
                zip.write(ByteArray(4))
                zip.closeEntry()
            }
        }.toByteArray()

        val failure = runCatching {
            bundles.read(ByteArrayInputStream(noMetadata), "bare", nowMillis = 1)
        }.exceptionOrNull()

        assertThat(failure).isInstanceOf(ParallaxException::class.java)
        assertThat(storage.exists("bare")).isFalse()
    }

    @Test
    fun `a bundle from a newer format version is refused with its version named`() {
        val newer = ByteArrayOutputStream().also { out ->
            ZipOutputStream(out).use { zip ->
                zip.putNextEntry(ZipEntry(ProjectBundle.METADATA))
                zip.write(validMetadataJson(version = BundleMetadata.VERSION + 1).toByteArray())
                zip.closeEntry()
            }
        }.toByteArray()

        val failure = runCatching {
            bundles.read(ByteArrayInputStream(newer), "future", nowMillis = 1)
        }.exceptionOrNull() as ParallaxException
        val error = failure.error as ParallaxError.BundleInvalid

        assertThat(error.detail).contains("${BundleMetadata.VERSION + 1}")
        assertThat(storage.exists("future")).isFalse()
    }

    @Test
    fun `a truncated archive leaves nothing behind`() {
        val bytes = export(includeRawFrames = true)
        val truncated = bytes.copyOf(bytes.size / 2)
        runCatching { bundles.read(ByteArrayInputStream(truncated), "half", nowMillis = 1) }
        assertThat(storage.exists("half")).isFalse()
    }

    private fun validMetadataJson(version: Int = BundleMetadata.VERSION): String =
        kotlinx.serialization.json.Json.encodeToString(
            BundleMetadata(version = version, exportedAtMillis = 1, project = project),
        )
}
