package uk.co.jacoblevy.parallax.feature.processing

import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.work.Configuration
import androidx.work.testing.SynchronousExecutor
import androidx.work.testing.WorkManagerTestInitHelper
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
import uk.co.jacoblevy.parallax.core.data.repository.ProcessingRepository
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.repository.TimeSource
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.data.storage.ProjectBundle
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.CaptureSummary
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ReconstructionStage
import uk.co.jacoblevy.parallax.core.testing.DirectExecutor
import uk.co.jacoblevy.parallax.core.testing.MainDispatcherRule
import java.io.File

@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
class ProcessingViewModelTest {

    @get:Rule
    val mainDispatcher = MainDispatcherRule()

    private lateinit var database: ParallaxDatabase
    private lateinit var storage: ProjectStorage
    private lateinit var projects: ProjectRepository
    private lateinit var processing: ProcessingRepository
    private lateinit var scheduler: ProcessingScheduler
    private var nextId = 0

    @Before
    fun setUp() {
        val context = ApplicationProvider.getApplicationContext<android.content.Context>()
        // WorkManager has to be initialised before the scheduler touches it, and the
        // synchronous executor keeps enqueueing deterministic.
        WorkManagerTestInitHelper.initializeTestWorkManager(
            context,
            Configuration.Builder()
                .setExecutor(SynchronousExecutor())
                .setMinimumLoggingLevel(android.util.Log.DEBUG)
                .build(),
        )

        database = Room.inMemoryDatabaseBuilder(context, ParallaxDatabase::class.java)
            .setQueryExecutor(DirectExecutor)
            .setTransactionExecutor(DirectExecutor)
            .allowMainThreadQueries()
            .build()
        storage = ProjectStorage(context)
        projects = ProjectRepository(
            projectDao = database.projectDao(),
            annotationDao = database.annotationDao(),
            measurementDao = database.measurementDao(),
            storage = storage,
            bundles = ProjectBundle(storage),
            time = TimeSource { 1_000L },
            io = Dispatchers.Unconfined,
        )
        processing = ProcessingRepository(
            database.processingJobDao(),
            TimeSource { 1_000L },
            Dispatchers.Unconfined,
        )
        val dir = File(context.filesDir, "processing-test").apply { mkdirs() }
        File(dir, "settings.preferences_pb").delete()
        val settings = SettingsRepository(
            PreferenceDataStoreFactory.create(
                scope = CoroutineScope(mainDispatcher.dispatcher + Job()),
            ) { File(dir, "settings.preferences_pb") },
        )
        scheduler = ProcessingScheduler(
            context = context,
            projects = projects,
            processing = processing,
            settings = settings,
            storage = storage,
            ids = IdGenerator { "job-${nextId++}" },
        )
    }

    @After
    fun tearDown() {
        database.close()
        storage.root.deleteRecursively()
    }

    private fun viewModel() = ProcessingViewModel(projects, processing, scheduler)

    private fun settled(model: ProcessingViewModel): ProcessingUiState {
        mainDispatcher.scheduler.advanceUntilIdle()
        return model.state.value
    }

    /** A scan with a manifest and enough frames: i.e. one that can actually be processed. */
    private suspend fun captured(id: String, name: String = id): Project {
        val project = projects.create(id, name, CaptureMode.Object, QualityPreset.Balanced)
        storage.framesDir(id).resolve("000000.jpg").writeBytes(ByteArray(64))
        storage.framesDir(id).resolve("000001.jpg").writeBytes(ByteArray(64))
        return projects.finishCapture(
            id = id,
            summary = CaptureSummary(keyframeCount = 40, hasArPoses = true),
            frames = emptyList(),
        )
    }

    @Test
    fun `an empty queue is reported as empty`() = runTest(mainDispatcher.dispatcher) {
        val state = settled(viewModel())
        assertThat(state.loading).isFalse()
        assertThat(state.isEmpty).isTrue()
    }

    @Test
    fun `queueing a scan puts it in the queue with a position`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val project = captured("p1", "Bench")
        scheduler.enqueue(project)

        val state = settled(viewModel())

        assertThat(state.queue).hasSize(1)
        assertThat(state.queue.single().project.name).isEqualTo("Bench")
        assertThat(state.queue.single().position).isEqualTo(1)
        assertThat(projects.get("p1")?.status).isInstanceOf(ProjectStatus.Queued::class.java)
    }

    @Test
    fun `queue positions reflect the order scans were added`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        scheduler.enqueue(captured("p1", "First"))
        scheduler.enqueue(captured("p2", "Second"))
        scheduler.enqueue(captured("p3", "Third"))

        val state = settled(viewModel())

        assertThat(state.queue.map { it.project.name })
            .containsExactly("First", "Second", "Third").inOrder()
        assertThat(state.queue.map { it.position }).containsExactly(1, 2, 3).inOrder()
    }

    @Test
    fun `a capture with no manifest is refused rather than queued to fail later`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        // Finding out twenty minutes later that a scan was never processable is a much worse
        // experience than being told now.
        val unusable = projects.create("p1", "Empty", CaptureMode.Object, QualityPreset.Fast)

        val failure = runCatching { scheduler.enqueue(unusable) }.exceptionOrNull()

        assertThat(failure).isNotNull()
        assertThat(processing.pending()).isEmpty()
    }

    @Test
    fun `queueing the same scan twice does not queue it twice`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val project = captured("p1")
        scheduler.enqueue(project)
        scheduler.enqueue(project)

        assertThat(settled(viewModel()).queue).hasSize(1)
    }

    @Test
    fun `cancelling removes it from the queue and restores its status`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        scheduler.enqueue(captured("p1"))
        val model = viewModel()
        settled(model)

        model.onEvent(ProcessingEvent.Cancel("p1"))

        val state = settled(model)
        assertThat(state.queue).isEmpty()
        assertThat(projects.get("p1")?.status).isEqualTo(ProjectStatus.Captured)
    }

    @Test
    fun `a running job is separated from the ones still waiting`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        scheduler.enqueue(captured("p1", "Running"))
        scheduler.enqueue(captured("p2", "Waiting"))
        val job = requireNotNull(processing.forProject("p1"))
        processing.markStarted(job.id)
        projects.updateStatus(
            "p1",
            ProjectStatus.Processing(
                stage = ReconstructionStage.Fusion,
                stageProgress = 0.5f,
                overallProgress = 0.42f,
                etaMillis = 90_000,
            ),
        )

        val state = settled(viewModel())

        assertThat(state.running?.project?.name).isEqualTo("Running")
        assertThat(state.queue.filterNot { it.isRunning }.map { it.project.name })
            .containsExactly("Waiting")
    }

    @Test
    fun `a failed scan is listed with its reason and can be retried`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        captured("p1", "Hard scan")
        projects.updateStatus(
            "p1",
            ProjectStatus.Failed(
                stage = ReconstructionStage.Fusion,
                reason = "ran out of voxel blocks",
                resumableFrom = ReconstructionStage.Depth,
            ),
        )
        val model = viewModel()
        assertThat(settled(model).failed.map { it.name }).containsExactly("Hard scan")

        model.onEvent(ProcessingEvent.Retry("p1"))

        val state = settled(model)
        assertThat(state.queue.map { it.project.id }).contains("p1")
    }

    @Test
    fun `retrying at a different quality records the new preset`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        captured("p1")
        val model = viewModel()
        settled(model)

        model.onEvent(ProcessingEvent.RetryAtQuality("p1", QualityPreset.Fast))

        settled(model)
        assertThat(projects.get("p1")?.quality).isEqualTo(QualityPreset.Fast)
        assertThat(processing.forProject("p1")?.quality).isEqualTo(QualityPreset.Fast)
    }

    @Test
    fun `finished scans are surfaced so the run has a visible result`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        captured("p1", "Done")
        storage.outputsDir("p1").resolve("model.glb").writeBytes(ByteArray(32))
        projects.recordOutputs("p1", setOf(OutputKind.Mesh), measurements = null)

        val state = settled(viewModel())

        assertThat(state.recentlyFinished.map { it.name }).containsExactly("Done")
        assertThat(state.isEmpty).isFalse()
    }

    @Test
    fun `an action on a missing project reports an error rather than failing silently`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)

        model.onEvent(ProcessingEvent.Retry("does-not-exist"))

        val state = settled(model)
        assertThat(state.error).isNotNull()
        model.onEvent(ProcessingEvent.DismissError)
        assertThat(settled(model).error).isNull()
    }

    @Test
    fun `startup recovery releases a job orphaned by a process kill`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        scheduler.enqueue(captured("p1"))
        val job = requireNotNull(processing.forProject("p1"))
        processing.markStarted(job.id)
        processing.markStageComplete(job.id, ReconstructionStage.Depth)
        assertThat(processing.forProject("p1")?.isRunning).isTrue()

        // Creating the ViewModel is what triggers recovery: nothing else would ever pick the
        // job up again, and the queue would sit behind it forever.
        settled(viewModel())

        val recovered = requireNotNull(processing.forProject("p1"))
        assertThat(recovered.isRunning).isFalse()
        // The progress it had made survives, so the retry resumes rather than restarting.
        assertThat(recovered.completedStage).isEqualTo(ReconstructionStage.Depth)
        assertThat(recovered.resumeFrom).isEqualTo(ReconstructionStage.Fusion)
    }

    @Test
    fun `a waiting row explains what it is waiting for`() = runTest(mainDispatcher.dispatcher) {
        scheduler.enqueue(captured("p1"))
        scheduler.enqueue(captured("p2"))

        val state = settled(viewModel())

        val second = state.queue.first { it.project.id == "p2" }
        assertThat(second.waitingReason).isNotNull()
        assertThat(second.waitingReason).contains("2")
    }
}
