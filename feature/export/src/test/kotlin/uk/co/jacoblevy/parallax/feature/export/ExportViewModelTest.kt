package uk.co.jacoblevy.parallax.feature.export

import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.lifecycle.SavedStateHandle
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.data.database.ParallaxDatabase
import uk.co.jacoblevy.parallax.core.data.repository.IdGenerator
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.repository.TimeSource
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.data.storage.ProjectBundle
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.data.storage.ProjectTransfer
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.ExportFormat
import uk.co.jacoblevy.parallax.core.model.LengthUnit
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.testing.DirectExecutor
import uk.co.jacoblevy.parallax.core.testing.MainDispatcherRule
import java.io.File

@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
class ExportViewModelTest {

    @get:Rule
    val mainDispatcher = MainDispatcherRule()

    private lateinit var database: ParallaxDatabase
    private lateinit var storage: ProjectStorage
    private lateinit var projects: ProjectRepository
    private lateinit var settings: SettingsRepository
    private lateinit var transfer: ProjectTransfer
    private lateinit var exporter: Exporter

    @Before
    fun setUp() {
        val context = ApplicationProvider.getApplicationContext<android.content.Context>()
        database = Room.inMemoryDatabaseBuilder(context, ParallaxDatabase::class.java)
            .setQueryExecutor(DirectExecutor)
            .setTransactionExecutor(DirectExecutor)
            .allowMainThreadQueries()
            .build()
        storage = ProjectStorage(context)
        val time = TimeSource { 1_000L }
        projects = ProjectRepository(
            projectDao = database.projectDao(),
            annotationDao = database.annotationDao(),
            measurementDao = database.measurementDao(),
            storage = storage,
            bundles = ProjectBundle(storage),
            time = time,
            io = Dispatchers.Unconfined,
        )
        val dir = File(context.filesDir, "export-test").apply { mkdirs() }
        File(dir, "settings.preferences_pb").delete()
        settings = SettingsRepository(
            PreferenceDataStoreFactory.create(
                scope = CoroutineScope(mainDispatcher.dispatcher + Job()),
            ) { File(dir, "settings.preferences_pb") },
        )
        transfer = ProjectTransfer(
            context = context,
            projects = projects,
            ids = IdGenerator { "generated" },
            storage = storage,
            io = Dispatchers.Unconfined,
        )
        exporter = Exporter(storage, Dispatchers.Unconfined)
    }

    @After
    fun tearDown() {
        database.close()
        storage.root.deleteRecursively()
    }

    private suspend fun seed(outputs: Set<OutputKind>): Project {
        val project = projects.create("p1", "Front room", CaptureMode.Room, QualityPreset.Balanced)
        projects.upsert(project.copy(outputs = outputs))
        return projects.require(project.id)
    }

    private fun viewModel(projectId: String) = ExportViewModel(
        savedState = SavedStateHandle(mapOf("projectId" to projectId)),
        projects = projects,
        settings = settings,
        storage = storage,
        transfer = transfer,
        exporter = exporter,
    )

    private fun settled(model: ExportViewModel): ExportUiState {
        mainDispatcher.scheduler.advanceUntilIdle()
        return model.state.value
    }

    @Test
    fun `a mesh-only scan is offered the mesh formats and told why the others are missing`() =
        runTest {
            val project = seed(setOf(OutputKind.Mesh))
            val state = settled(viewModel(project.id))

            assertThat(state.formats).contains(ExportFormat.Glb)
            assertThat(state.formats).contains(ExportFormat.Usdz)
            assertThat(state.formats).contains(ExportFormat.HtmlViewer)
            // Listed, not hidden: someone looking for SPZ needs to know the
            // scan has no splat field rather than that the app lacks the format.
            assertThat(state.unavailable).contains(ExportFormat.Spz)
            assertThat(state.unavailable).contains(ExportFormat.FloorPlanPdf)
            assertThat(state.formats + state.unavailable)
                .containsExactlyElementsIn(ExportFormat.entries)
        }

    @Test
    fun `a scan with everything is offered everything`() = runTest {
        val project = seed(
            setOf(
                OutputKind.Mesh,
                OutputKind.TexturedMesh,
                OutputKind.WatertightMesh,
                OutputKind.PointCloud,
                OutputKind.Splat,
                OutputKind.FloorPlan,
            ),
        )
        val state = settled(viewModel(project.id))
        assertThat(state.unavailable).isEmpty()
    }

    @Test
    fun `the unit choice starts from the user's setting`() = runTest {
        settings.setUnits(LengthUnit.Feet)
        val project = seed(setOf(OutputKind.Mesh))
        assertThat(settled(viewModel(project.id)).settings.unit).isEqualTo(LengthUnit.Feet)
    }

    @Test
    fun `a missing project says so rather than offering an empty format list`() = runTest {
        val state = settled(viewModel("nope"))
        assertThat(state.project).isNull()
        assertThat(state.formats).isEmpty()
        assertThat(state.error).isNotEmpty()
    }

    @Test
    fun `options are remembered as they are changed`() = runTest {
        val project = seed(setOf(OutputKind.Mesh))
        val model = viewModel(project.id)
        settled(model)

        model.setUnit(LengthUnit.Millimetres)
        model.setZUp(true)
        model.setDarkFloorPlan(false)
        model.setIncludeRawFrames(true)
        model.setDestination(ExportDestination.SaveToFiles)

        val state = settled(model)
        assertThat(state.settings.unit).isEqualTo(LengthUnit.Millimetres)
        assertThat(state.settings.zUp).isTrue()
        assertThat(state.settings.darkFloorPlan).isFalse()
        assertThat(state.includeRawFramesInBundle).isTrue()
        assertThat(state.destination).isEqualTo(ExportDestination.SaveToFiles)
    }

    @Test
    fun `saving to files asks the system where, with a sensible name`() = runTest {
        val project = seed(setOf(OutputKind.Mesh))
        val model = viewModel(project.id)
        settled(model)
        model.setDestination(ExportDestination.SaveToFiles)

        val events = mutableListOf<ExportEvent>()
        val job = kotlinx.coroutines.CoroutineScope(mainDispatcher.dispatcher).launch {
            model.events.collect { events.add(it) }
        }
        mainDispatcher.scheduler.advanceUntilIdle()

        model.export(ExportFormat.Glb)
        mainDispatcher.scheduler.advanceUntilIdle()

        assertThat(events).hasSize(1)
        val event = events.first() as ExportEvent.ChooseDestination
        assertThat(event.suggestedName).isEqualTo("Front-room.glb")
        assertThat(event.mimeType).isEqualTo("model/gltf-binary")
        // Nothing has been written yet: a cancelled dialog must leave no file.
        assertThat(settled(model).busyWith).isNull()
        job.cancel()
    }

    @Test
    fun `a cancelled save dialog is not an error`() = runTest {
        val project = seed(setOf(OutputKind.Mesh))
        val model = viewModel(project.id)
        settled(model)
        model.setDestination(ExportDestination.SaveToFiles)
        model.export(ExportFormat.Glb)
        mainDispatcher.scheduler.advanceUntilIdle()

        model.writeTo(null)
        val state = settled(model)
        assertThat(state.error).isNull()
        assertThat(state.busyWith).isNull()
    }

    @Test
    fun `a format the scan cannot produce is refused with a reason`() = runTest {
        val project = seed(setOf(OutputKind.Mesh))
        val model = viewModel(project.id)
        settled(model)
        // Sharing an SPZ from a scan with no splat field: the engine is not
        // even reached, because the output set already says it is impossible.
        model.export(ExportFormat.Spz)
        val state = settled(model)
        assertThat(state.error).isNotEmpty()
        assertThat(state.busyWith).isNull()
    }

    @Test
    fun `an export that fails reports why and stops being busy`() = runTest {
        val project = seed(setOf(OutputKind.Mesh))
        val model = viewModel(project.id)
        settled(model)
        // There is no native library on a JVM host and no mesh on disk, so the
        // GLB write cannot succeed. What matters is that it says so.
        model.export(ExportFormat.Glb)
        val state = settled(model)
        assertThat(state.error).isNotEmpty()
        assertThat(state.busyWith).isNull()
    }

    @Test
    fun `a message can be dismissed`() = runTest {
        val project = seed(setOf(OutputKind.Mesh))
        val model = viewModel(project.id)
        settled(model)
        model.export(ExportFormat.Spz)
        assertThat(settled(model).error).isNotNull()
        model.dismissMessage()
        assertThat(settled(model).error).isNull()
    }
}

@RunWith(RobolectricTestRunner::class)
class ExporterNamingTest {

    private fun project(name: String) = Project(
        id = "p",
        name = name,
        captureMode = CaptureMode.Object,
        createdAtMillis = 0,
        updatedAtMillis = 0,
        status = uk.co.jacoblevy.parallax.core.model.ProjectStatus.Captured,
    )

    private val exporter = Exporter(
        storage = ProjectStorage(ApplicationProvider.getApplicationContext()),
        io = Dispatchers.Unconfined,
    )

    @Test
    fun `names are safe for any filesystem`() {
        // A scan name is whatever the user typed, including slashes.
        assertThat(exporter.suggestedName(project("Jo's kitchen / 2nd floor"), ExportFormat.Glb))
            .isEqualTo("Jo-s-kitchen-2nd-floor.glb")
    }

    @Test
    fun `an unnamed scan still gets a file name`() {
        assertThat(exporter.suggestedName(project("   "), ExportFormat.Obj))
            .isEqualTo("parallax-scan.obj")
        assertThat(exporter.suggestedName(project("///"), ExportFormat.Obj))
            .isEqualTo("parallax-scan.obj")
    }

    @Test
    fun `the multi-file gltf layout is named as a folder`() {
        assertThat(exporter.suggestedName(project("Chair"), ExportFormat.Gltf))
            .isEqualTo("Chair")
        assertThat(exporter.suggestedName(project("Chair"), ExportFormat.Glb))
            .isEqualTo("Chair.glb")
    }
}
