package uk.co.jacoblevy.parallax.feature.viewer

import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.lifecycle.SavedStateHandle
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
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
import uk.co.jacoblevy.parallax.core.model.BoundingBox
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.LightingPreset
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.ModelEdit
import uk.co.jacoblevy.parallax.core.model.ModelMeasurements
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.RenderMode
import uk.co.jacoblevy.parallax.core.testing.DirectExecutor
import uk.co.jacoblevy.parallax.core.testing.MainDispatcherRule
import uk.co.jacoblevy.parallax.feature.viewer.measure.MeasureTool
import uk.co.jacoblevy.parallax.feature.viewer.render.ModelLoader
import java.io.File

@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
class ViewerViewModelTest {

    @get:Rule
    val mainDispatcher = MainDispatcherRule()

    private lateinit var database: ParallaxDatabase
    private lateinit var storage: ProjectStorage
    private lateinit var projects: ProjectRepository
    private lateinit var settings: SettingsRepository
    private lateinit var loader: ModelLoader
    private var nextId = 0

    private val bounds = BoundingBox(-1f, 0f, -2f, 1f, 2f, 2f)

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
        val dir = File(context.filesDir, "viewer-test").apply { mkdirs() }
        File(dir, "settings.preferences_pb").delete()
        settings = SettingsRepository(
            PreferenceDataStoreFactory.create(
                scope = CoroutineScope(mainDispatcher.dispatcher + Job()),
            ) { File(dir, "settings.preferences_pb") },
        )
        loader = ModelLoader(storage, Dispatchers.Unconfined)
    }

    @After
    fun tearDown() {
        database.close()
        storage.root.deleteRecursively()
    }

    private suspend fun seed(
        outputs: Set<OutputKind> = setOf(OutputKind.Mesh, OutputKind.TexturedMesh),
    ): String {
        val project = projects.create("p1", "Kitchen", CaptureMode.Room, QualityPreset.Balanced)
        projects.upsert(
            project.copy(
                outputs = outputs,
                measurements = ModelMeasurements(triangleCount = 1000, boundsMetres = bounds),
            ),
        )
        return project.id
    }

    private fun viewModel(projectId: String) = ViewerViewModel(
        savedState = SavedStateHandle(mapOf("projectId" to projectId)),
        projects = projects,
        settings = settings,
        loader = loader,
        ids = IdGenerator { "id-${nextId++}" },
    )

    private fun settled(model: ViewerViewModel): ViewerUiState {
        mainDispatcher.scheduler.advanceUntilIdle()
        return model.state.value
    }

    @Test
    fun `a missing project says so rather than showing an empty viewer`() = runTest {
        val state = settled(viewModel("nope"))
        assertThat(state.loading).isFalse()
        assertThat(state.project).isNull()
        assertThat(state.message).isNotEmpty()
    }

    @Test
    fun `only the render modes this scan can offer are listed`() = runTest {
        val id = seed(setOf(OutputKind.Mesh, OutputKind.PointCloud))
        val state = settled(viewModel(id))
        assertThat(state.availableModes)
            .containsExactly(RenderMode.Shaded, RenderMode.Wireframe, RenderMode.PointCloud)
        assertThat(state.availableModes).doesNotContain(RenderMode.Textured)
        assertThat(state.availableModes).doesNotContain(RenderMode.Splat)
    }

    @Test
    fun `the camera opens framed on the model`() = runTest {
        val id = seed()
        val state = settled(viewModel(id))
        // Centred on the bounding box, and far enough out to see all of it.
        assertThat(state.orbit.targetX).isWithin(1e-4f).of(0f)
        assertThat(state.orbit.targetY).isWithin(1e-4f).of(1f)
        assertThat(state.orbit.distance).isGreaterThan(bounds.depth / 2f)
    }

    @Test
    fun `resetting the view puts the camera back where it started`() = runTest {
        val id = seed()
        val model = viewModel(id)
        val opening = settled(model).orbit
        model.rotate(1f, 0.4f)
        model.zoom(2f)
        assertThat(settled(model).orbit).isNotEqualTo(opening)
        model.resetView()
        assertThat(settled(model).orbit).isEqualTo(opening)
    }

    @Test
    fun `a drag stops any fling that was still running`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        model.fling(4f, 0f)
        assertThat(settled(model).orbit.isSettled).isFalse()
        model.rotate(0.1f, 0f)
        assertThat(settled(model).orbit.isSettled).isTrue()
    }

    @Test
    fun `panning and zooming do nothing in walk mode`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        model.setWalking(true)
        val before = settled(model).orbit
        model.pan(0.5f, 0.5f)
        model.zoom(2f)
        assertThat(settled(model).orbit).isEqualTo(before)
    }

    @Test
    fun `walk mode starts in the middle of the room and cannot leave it`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        model.setWalking(true)
        assertThat(settled(model).walk.x).isWithin(1e-4f).of(0f)

        repeat(100) { model.walk(1f, 0f) }
        val walked = settled(model).walk
        assertThat(walked.z).isAtMost(bounds.maxZ + 1f)
        assertThat(walked.x).isAtMost(bounds.maxX + 1f)
    }

    @Test
    fun `entering walk mode leaves the look tool selected`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        model.setInteraction(ViewerInteraction.Measure)
        model.setWalking(true)
        // Measuring by tapping needs a stationary camera you can aim.
        assertThat(settled(model).interaction).isEqualTo(ViewerInteraction.Look)
    }

    @Test
    fun `lighting and brightness are remembered and brightness is bounded`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        model.setLighting(LightingPreset.Flat)
        model.setExposure(9f)
        val state = settled(model)
        assertThat(state.lighting).isEqualTo(LightingPreset.Flat)
        assertThat(state.exposureStops).isEqualTo(2f)
        model.setExposure(-9f)
        assertThat(settled(model).exposureStops).isEqualTo(-2f)
    }

    @Test
    fun `choosing the measure tool starts a fresh draft`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        model.setInteraction(ViewerInteraction.Measure)
        assertThat(settled(model).draft?.tool).isEqualTo(MeasureTool.Distance)
        model.setMeasureTool(MeasureTool.Area)
        val state = settled(model)
        assertThat(state.draft?.tool).isEqualTo(MeasureTool.Area)
        assertThat(state.draft?.points).isEmpty()
    }

    @Test
    fun `leaving the measure tool discards the half-finished draft`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        model.setInteraction(ViewerInteraction.Measure)
        model.setInteraction(ViewerInteraction.Look)
        assertThat(settled(model).draft).isNull()
    }

    @Test
    fun `a tap with no model under it is reported rather than ignored`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        model.setInteraction(ViewerInteraction.Measure)
        // There is no mesh on disk in this test, so there is nothing to pick.
        assertThat(model.tap(10f, 10f, 100, 100)).isFalse()
    }

    @Test
    fun `taps do nothing while looking`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        assertThat(model.tap(10f, 10f, 100, 100)).isFalse()
    }

    @Test
    fun `saved measurements come back and can be deleted`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        projects.upsertMeasurement(
            id,
            Measurement.Distance("m1", "Width", 0f, 0f, 0f, 2f, 0f, 0f),
        )
        assertThat(settled(model).measurements).hasSize(1)
        model.deleteMeasurement("m1")
        assertThat(settled(model).measurements).isEmpty()
    }

    @Test
    fun `writing a note needs a point on the model first`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        // No pending point, so saving is a no-op rather than a pin at the origin.
        model.saveAnnotation("Damp", "Behind the radiator")
        assertThat(settled(model).annotations).isEmpty()
    }

    @Test
    fun `edits are applied in order, undone one at a time, and persisted`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)

        model.applyEdit(ModelEdit.CropBox(BoundingBox(-1f, 0f, -1f, 1f, 1f, 1f)))
        model.applyEdit(ModelEdit.Rescale(measuredMetres = 1.0, actualMetres = 2.0))
        var state = settled(model)
        assertThat(state.edits).hasSize(2)
        assertThat(state.canUndoEdit).isTrue()
        assertThat(state.editPlan.transformed(1f, 0f, 0f)[0]).isWithin(1e-4f).of(2f)

        model.undoEdit()
        state = settled(model)
        assertThat(state.edits).hasSize(1)
        // And they survive the viewer being closed and opened again.
        assertThat(projects.get(id)?.edits).hasSize(1)
    }

    @Test
    fun `rescaling from a measurement uses the measurement's own length`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        projects.upsertMeasurement(
            id,
            Measurement.Distance("m1", "Door", 0f, 0f, 0f, 1f, 0f, 0f),
        )
        settled(model)

        model.rescaleFrom("m1", actualMetres = 2.1)
        val plan = settled(model).editPlan
        assertThat(plan.transformed(1f, 0f, 0f)[0]).isWithin(1e-4f).of(2.1f)
    }

    @Test
    fun `rescaling from a measurement that is not a length is refused`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        projects.upsertMeasurement(
            id,
            Measurement.Area("a1", "Floor", listOf()),
        )
        settled(model)
        model.rescaleFrom("a1", actualMetres = 3.0)
        assertThat(settled(model).edits).isEmpty()
    }

    @Test
    fun `rescaling to nothing is refused rather than collapsing the model`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        projects.upsertMeasurement(
            id,
            Measurement.Distance("m1", "Door", 0f, 0f, 0f, 1f, 0f, 0f),
        )
        settled(model)
        model.rescaleFrom("m1", actualMetres = 0.0)
        assertThat(settled(model).edits).isEmpty()
    }

    @Test
    fun `a renderer that will not start is reflected in the state`() = runTest {
        val id = seed()
        val model = viewModel(id)
        settled(model)
        assertThat(settled(model).rendererFailed).isFalse()
        model.rendererFailed()
        assertThat(settled(model).rendererFailed).isTrue()
    }

    @Test
    fun `a model that cannot be prepared leaves a message and no file`() = runTest {
        val id = seed()
        val state = settled(viewModel(id))
        // Nothing was ever written to disk for this project, so the loader
        // fails; the viewer must say so rather than showing a blank scene.
        assertThat(state.modelFile).isNull()
        assertThat(state.message).isNotNull()
    }
}
