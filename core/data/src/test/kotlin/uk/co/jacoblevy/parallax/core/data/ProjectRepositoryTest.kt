package uk.co.jacoblevy.parallax.core.data

import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.common.result.ParallaxException
import uk.co.jacoblevy.parallax.core.data.database.ParallaxDatabase
import uk.co.jacoblevy.parallax.core.data.repository.FolderRepository
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.repository.TimeSource
import uk.co.jacoblevy.parallax.core.data.storage.ProjectBundle
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.Annotation
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.CaptureSummary
import uk.co.jacoblevy.parallax.core.model.LibraryQuery
import uk.co.jacoblevy.parallax.core.model.LibrarySort
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.ModelMeasurements
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.Point3
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream

@RunWith(RobolectricTestRunner::class)
class ProjectRepositoryTest {

    private lateinit var database: ParallaxDatabase
    private lateinit var storage: ProjectStorage
    private lateinit var repository: ProjectRepository
    private lateinit var folders: FolderRepository
    private var now = 1_000L

    @Before
    fun setUp() {
        val context = ApplicationProvider.getApplicationContext<android.content.Context>()
        database = Room.inMemoryDatabaseBuilder(context, ParallaxDatabase::class.java)
            .allowMainThreadQueries()
            .build()
        storage = ProjectStorage(context)
        val time = TimeSource { now }
        repository = ProjectRepository(
            projectDao = database.projectDao(),
            annotationDao = database.annotationDao(),
            measurementDao = database.measurementDao(),
            storage = storage,
            bundles = ProjectBundle(storage),
            time = time,
            io = kotlinx.coroutines.Dispatchers.Unconfined,
        )
        folders = FolderRepository(database.folderDao(), time, kotlinx.coroutines.Dispatchers.Unconfined)
    }

    @After
    fun tearDown() {
        database.close()
        storage.root.deleteRecursively()
    }

    private suspend fun newProject(id: String, name: String = id) = repository.create(
        id = id,
        name = name,
        mode = CaptureMode.Object,
        quality = QualityPreset.Balanced,
    )

    @Test
    fun `creating a project creates its directory and its row together`() = runTest {
        val project = newProject("p1", "Chair")

        assertThat(storage.exists("p1")).isTrue()
        assertThat(repository.get("p1")).isEqualTo(project)
        assertThat(project.status).isEqualTo(ProjectStatus.Capturing(frameCount = 0))
    }

    @Test
    fun `deleting a project removes the row and the directory`() = runTest {
        newProject("p1")
        storage.outputsDir("p1").resolve("model.glb").writeBytes(ByteArray(10))

        repository.delete("p1")

        assertThat(repository.get("p1")).isNull()
        assertThat(storage.exists("p1")).isFalse()
    }

    @Test
    fun `asking for a missing project reports it as missing rather than returning null`() = runTest {
        val failure = runCatching { repository.require("nope") }.exceptionOrNull()
        assertThat(failure).isInstanceOf(ParallaxException::class.java)
    }

    @Test
    fun `finishing capture writes the manifest the pipeline will read`() = runTest {
        newProject("p1")
        val frames = TestFixtures.frames(count = 6)

        val updated = repository.finishCapture(
            id = "p1",
            summary = CaptureSummary(frameCount = 6, keyframeCount = 3, coverage = 0.6f),
            frames = frames,
        )

        assertThat(updated.status).isEqualTo(ProjectStatus.Captured)
        assertThat(updated.capture.frameCount).isEqualTo(6)
        assertThat(storage.readManifest("p1")?.frames).hasSize(6)
        assertThat(storage.readManifest("p1")?.hasArPoses).isTrue()
    }

    @Test
    fun `recording outputs makes the project viewable and stores its measurements`() = runTest {
        newProject("p1")
        storage.outputsDir("p1").resolve("model.glb").writeBytes(ByteArray(2_048))

        val updated = repository.recordOutputs(
            id = "p1",
            outputs = setOf(OutputKind.Mesh, OutputKind.TexturedMesh),
            measurements = ModelMeasurements(triangleCount = 1_234, watertight = true),
        )

        assertThat(updated.hasViewableOutput).isTrue()
        assertThat(updated.measurements?.triangleCount).isEqualTo(1_234)
        // Size is read from disk, not passed in, so it cannot drift from reality.
        assertThat(updated.storageBytes).isEqualTo(2_048)
    }

    @Test
    fun `purging intermediates frees space and updates the recorded size`() = runTest {
        newProject("p1")
        storage.framesDir("p1").resolve("f.jpg").writeBytes(ByteArray(4_000))
        storage.outputsDir("p1").resolve("model.glb").writeBytes(ByteArray(1_000))
        repository.refreshStorageBytes("p1")

        val freed = repository.purgeIntermediates("p1")

        assertThat(freed).isEqualTo(4_000)
        assertThat(repository.get("p1")?.storageBytes).isEqualTo(1_000)
    }

    @Test
    fun `total storage is the sum over projects`() = runTest {
        newProject("p1")
        newProject("p2")
        storage.outputsDir("p1").resolve("a").writeBytes(ByteArray(100))
        storage.outputsDir("p2").resolve("a").writeBytes(ByteArray(250))
        repository.refreshStorageBytes("p1")
        repository.refreshStorageBytes("p2")

        assertThat(repository.observeTotalStorageBytes().first()).isEqualTo(350)
    }

    @Test
    fun `reconcile reclaims a directory whose row never got written`() = runTest {
        newProject("p1")
        storage.create("ghost")
        storage.outputsDir("ghost").resolve("x").writeBytes(ByteArray(600))

        val freed = repository.reconcile()

        assertThat(freed).isEqualTo(600)
        assertThat(storage.exists("ghost")).isFalse()
        assertThat(storage.exists("p1")).isTrue()
    }

    @Test
    fun `library search matches name and notes`() = runTest {
        newProject("p1", "Kitchen worktop")
        newProject("p2", "Shed")
        repository.setNotes("p2", "the kitchen one is better")

        val byName = repository.observeLibrary(flowOf(LibraryQuery(search = "worktop"))).first()
        assertThat(byName.map { it.id }).containsExactly("p1")

        val byNotes = repository.observeLibrary(flowOf(LibraryQuery(search = "KITCHEN"))).first()
        assertThat(byNotes.map { it.id }).containsExactly("p1", "p2")
    }

    @Test
    fun `favourites stay at the top of every sort order`() = runTest {
        now = 10
        newProject("older", "Aardvark")
        now = 20
        newProject("newer", "Zebra")
        repository.setFavourite("older", true)

        for (sort in LibrarySort.entries) {
            val ids = repository.observeLibrary(flowOf(LibraryQuery(sort = sort))).first()
                .map { it.id }
            assertThat(ids.first()).isEqualTo("older")
        }
    }

    @Test
    fun `library filters by folder and by mode`() = runTest {
        val folder = folders.create("f1", "Furniture")
        newProject("p1")
        repository.create("p2", "Lounge", CaptureMode.Room, QualityPreset.Fast)
        repository.moveToFolder("p1", folder.id)

        val inFolder = repository.observeLibrary(flowOf(LibraryQuery(folderId = "f1"))).first()
        assertThat(inFolder.map { it.id }).containsExactly("p1")

        val rooms = repository
            .observeLibrary(flowOf(LibraryQuery(modeFilter = CaptureMode.Room)))
            .first()
        assertThat(rooms.map { it.id }).containsExactly("p2")
    }

    @Test
    fun `deleting a folder leaves its projects unfiled rather than deleting them`() = runTest {
        folders.create("f1", "Furniture")
        newProject("p1")
        repository.moveToFolder("p1", "f1")

        folders.delete("f1")

        assertThat(repository.get("p1")).isNotNull()
        assertThat(repository.get("p1")?.folderId).isNull()
    }

    @Test
    fun `folder counts include empty folders`() = runTest {
        folders.create("f1", "Used")
        folders.create("f2", "Empty")
        newProject("p1")
        repository.moveToFolder("p1", "f1")

        val all = folders.observeAll().first().associateBy { it.id }

        assertThat(all.keys).containsExactly("f1", "f2")
        assertThat(all.getValue("f1").projectCount).isEqualTo(1)
        assertThat(all.getValue("f2").projectCount).isEqualTo(0)
    }

    @Test
    fun `annotations and measurements follow the project and die with it`() = runTest {
        newProject("p1")
        repository.upsertAnnotation(
            "p1",
            Annotation("a1", "Scratch", "on the lid", Point3(0f, 1f, 2f)),
        )
        repository.upsertMeasurement("p1", Measurement.Height("m1", "Back", 0f, 0.92f))

        assertThat(repository.observeAnnotations("p1").first()).hasSize(1)
        assertThat(repository.observeMeasurements("p1").first()).hasSize(1)

        repository.delete("p1")

        assertThat(repository.observeAnnotations("p1").first()).isEmpty()
        assertThat(repository.observeMeasurements("p1").first()).isEmpty()
    }

    @Test
    fun `an annotation with no timestamp is stamped on insert`() = runTest {
        newProject("p1")
        now = 7_777
        repository.upsertAnnotation("p1", Annotation("a1", "t", "b", Point3(0f, 0f, 0f)))
        assertThat(repository.observeAnnotations("p1").first().single().createdAtMillis)
            .isEqualTo(7_777)
    }

    @Test
    fun `a bundle exported and re-imported becomes a second independent project`() = runTest {
        newProject("p1", "Bench")
        storage.outputsDir("p1").resolve("model.glb").writeBytes(ByteArray(64))
        repository.recordOutputs("p1", setOf(OutputKind.Mesh), null)
        repository.upsertAnnotation("p1", Annotation("a1", "Knot", "", Point3(0f, 0f, 0f)))
        repository.upsertMeasurement("p1", Measurement.Height("m1", "H", 0f, 0.5f))

        val out = ByteArrayOutputStream()
        repository.exportBundle("p1", out, includeRawFrames = false)
        val imported = repository.importBundle(ByteArrayInputStream(out.toByteArray()), "p2")

        assertThat(imported.id).isEqualTo("p2")
        assertThat(imported.name).isEqualTo("Bench")
        assertThat(repository.observeAll().first().map { it.id }).containsExactly("p1", "p2")
        assertThat(repository.observeAnnotations("p2").first()).hasSize(1)
        assertThat(repository.observeMeasurements("p2").first()).hasSize(1)
        // The original keeps its own annotations; the import did not move them. Annotation ids
        // are table-wide primary keys, so the import has to re-id what it inserts.
        assertThat(repository.observeAnnotations("p1").first().single().id).isEqualTo("a1")
        assertThat(repository.observeAnnotations("p2").first().single().id).isEqualTo("p2:a1")
        assertThat(repository.observeMeasurements("p2").first().single().id).isEqualTo("p2:m1")
    }

    @Test
    fun `an imported project is unfiled even if the bundle named a folder`() = runTest {
        folders.create("f1", "Old device folder")
        newProject("p1")
        repository.moveToFolder("p1", "f1")
        storage.outputsDir("p1").resolve("model.glb").writeBytes(ByteArray(16))

        val out = ByteArrayOutputStream()
        repository.exportBundle("p1", out, includeRawFrames = false)
        // The foreign key would otherwise reject the row outright on a device without that
        // folder, which is the normal case for a bundle shared between people.
        val imported = repository.importBundle(ByteArrayInputStream(out.toByteArray()), "p2")

        assertThat(imported.folderId).isNull()
    }
}
