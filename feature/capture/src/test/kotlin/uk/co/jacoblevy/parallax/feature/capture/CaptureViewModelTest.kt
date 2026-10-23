package uk.co.jacoblevy.parallax.feature.capture

import androidx.datastore.preferences.core.PreferenceDataStoreFactory
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
import uk.co.jacoblevy.parallax.core.model.ArCoreAvailability
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.CaptureStats
import uk.co.jacoblevy.parallax.core.model.CoverageMap
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilityProvider
import uk.co.jacoblevy.parallax.core.model.GuidancePrompt
import uk.co.jacoblevy.parallax.core.model.HapticCue
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ThermalStatus
import uk.co.jacoblevy.parallax.core.testing.DirectExecutor
import uk.co.jacoblevy.parallax.core.testing.MainDispatcherRule
import uk.co.jacoblevy.parallax.feature.capture.session.CaptureTick
import uk.co.jacoblevy.parallax.feature.capture.session.DeviceConditionMonitor
import uk.co.jacoblevy.parallax.feature.capture.session.MediaImporter
import java.io.File

@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
class CaptureViewModelTest {

    @get:Rule
    val mainDispatcher = MainDispatcherRule()

    private lateinit var database: ParallaxDatabase
    private lateinit var storage: ProjectStorage
    private lateinit var projects: ProjectRepository
    private lateinit var settings: SettingsRepository
    private lateinit var importer: MediaImporter
    private lateinit var conditions: DeviceConditionMonitor

    private val playedCues = mutableListOf<HapticCue>()
    /** A capable device: enough RAM for Max quality, so the cap is not what is under test. */
    private var capabilities = DeviceCapabilities(
        arCoreAvailability = ArCoreAvailability.Supported,
        supportsDepthApi = true,
        supportsVulkanCompute = true,
        totalRamBytes = 12L * 1024 * 1024 * 1024,
    )
    private var nextId = 0

    @Before
    fun setUp() {
        val context = ApplicationProvider.getApplicationContext<android.content.Context>()
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
        val dir = File(context.filesDir, "capture-test").apply { mkdirs() }
        File(dir, "settings.preferences_pb").delete()
        settings = SettingsRepository(
            PreferenceDataStoreFactory.create(
                scope = CoroutineScope(mainDispatcher.dispatcher + Job()),
            ) { File(dir, "settings.preferences_pb") },
        )
        importer = MediaImporter(context, storage, Dispatchers.Unconfined)
        conditions = DeviceConditionMonitor(context)
    }

    @After
    fun tearDown() {
        database.close()
        storage.root.deleteRecursively()
    }

    private fun viewModel() = CaptureViewModel(
        projects = projects,
        storage = storage,
        settings = settings,
        capabilities = object : DeviceCapabilityProvider {
            override suspend fun resolve(): DeviceCapabilities = capabilities
        },
        conditions = conditions,
        ids = IdGenerator { "generated-${nextId++}" },
        importer = importer,
        haptics = { cue, enabled -> if (enabled) playedCues += cue },
    )

    private fun settled(model: CaptureViewModel): CaptureUiState {
        mainDispatcher.scheduler.advanceUntilIdle()
        return model.state.value
    }

    @Test
    fun `it opens on the stored default mode when the device supports it`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        settings.setDefaultCaptureMode(CaptureMode.Room)
        settings.setDefaultQuality(QualityPreset.Max)

        val state = settled(viewModel())

        assertThat(state.mode).isEqualTo(CaptureMode.Room)
        assertThat(state.quality).isEqualTo(QualityPreset.Max)
        assertThat(state.phase).isEqualTo(CapturePhase.Ready)
    }

    @Test
    fun `a stored mode the device cannot do falls back to one it can`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        settings.setDefaultCaptureMode(CaptureMode.Room)
        capabilities = DeviceCapabilities(
            arCoreAvailability = ArCoreAvailability.UnsupportedDevice,
        )

        val state = settled(viewModel())

        // Opening on a mode that cannot start would show a camera screen that does nothing.
        assertThat(state.mode).isEqualTo(CaptureMode.Import)
        assertThat(state.availableModes).containsExactly(CaptureMode.Import)
    }

    @Test
    fun `a stored quality above what the device can run is capped`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        settings.setDefaultQuality(QualityPreset.Max)
        capabilities = capabilities.copy(totalRamBytes = 3L * 1024 * 1024 * 1024)

        assertThat(settled(viewModel()).quality).isEqualTo(QualityPreset.Fast)
    }

    @Test
    fun `starting a session creates the project and its directory`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)

        model.onEvent(CaptureEvent.Start)

        val state = settled(model)
        assertThat(state.phase).isEqualTo(CapturePhase.Capturing)
        assertThat(state.projectId).isEqualTo("generated-0")
        assertThat(storage.exists("generated-0")).isTrue()
        assertThat(projects.get("generated-0")?.status)
            .isInstanceOf(ProjectStatus.Capturing::class.java)
        assertThat(model.engine).isNotNull()
        assertThat(playedCues).contains(HapticCue.Shutter)
    }

    @Test
    fun `a mode that needs AR on a device without it never starts`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        capabilities = DeviceCapabilities(
            arCoreAvailability = ArCoreAvailability.UnsupportedDevice,
        )
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.ModeSelected(CaptureMode.Object))

        model.onEvent(CaptureEvent.Start)

        val state = settled(model)
        assertThat(state.blockedByArCore).isTrue()
        assertThat(state.phase).isEqualTo(CapturePhase.Ready)
        assertThat(state.projectId).isNull()
    }

    @Test
    fun `the mode cannot be changed once recording has begun`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.ModeSelected(CaptureMode.Object))
        model.onEvent(CaptureEvent.Start)
        settled(model)

        model.onEvent(CaptureEvent.ModeSelected(CaptureMode.Room))

        // Switching mid-scan would mix two different capture strategies into one project.
        assertThat(settled(model).mode).isEqualTo(CaptureMode.Object)
    }

    @Test
    fun `pausing and resuming keeps the project and the engine`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.Start)
        settled(model)

        model.onEvent(CaptureEvent.Pause)
        assertThat(settled(model).phase).isEqualTo(CapturePhase.Paused)
        assertThat(model.engine).isNotNull()

        model.onEvent(CaptureEvent.Resume)
        assertThat(settled(model).phase).isEqualTo(CapturePhase.Capturing)
        assertThat(settled(model).projectId).isEqualTo("generated-0")
    }

    @Test
    fun `discarding deletes the project and its files`() = runTest(mainDispatcher.dispatcher) {
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.Start)
        settled(model)

        model.onEvent(CaptureEvent.Discard)

        val state = settled(model)
        assertThat(state.phase).isEqualTo(CapturePhase.Ready)
        assertThat(state.projectId).isNull()
        assertThat(projects.get("generated-0")).isNull()
        assertThat(storage.exists("generated-0")).isFalse()
    }

    @Test
    fun `a tick updates the stats, the coverage and the prompt`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.Start)
        settled(model)

        model.onFrameProcessed(
            CaptureTick(
                stats = CaptureStats(keyframeCount = 7, coverage = 0.3f),
                coverage = CoverageMap(bins = FloatArray(144) { 0.5f }),
                prompt = GuidancePrompt.MissedTheBack,
                promptChanged = true,
                recorded = null,
            ),
        )

        val state = settled(model)
        assertThat(state.stats.keyframeCount).isEqualTo(7)
        assertThat(state.prompt).isEqualTo(GuidancePrompt.MissedTheBack)
        assertThat(state.coverage.coverage).isGreaterThan(0f)
        // A changed prompt is what fires the haptic; an unchanged one must not.
        assertThat(playedCues).contains(GuidancePrompt.MissedTheBack.haptic)
    }

    @Test
    fun `an unchanged prompt does not fire a haptic`() = runTest(mainDispatcher.dispatcher) {
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.Start)
        settled(model)
        playedCues.clear()

        model.onFrameProcessed(
            CaptureTick(
                stats = CaptureStats(),
                coverage = CoverageMap(),
                prompt = GuidancePrompt.KeepGoing,
                promptChanged = false,
                recorded = null,
            ),
        )

        assertThat(playedCues).isEmpty()
    }

    @Test
    fun `the thermal status from the monitor is not overwritten by a stale tick`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.Start)
        settled(model)

        model.onFrameProcessed(
            CaptureTick(
                // A tick carries the engine's copy of the conditions, which can be a frame
                // behind the monitor's; the monitor is authoritative.
                stats = CaptureStats(thermalStatus = ThermalStatus.Critical, batteryPercent = 3),
                coverage = CoverageMap(),
                prompt = null,
                promptChanged = false,
                recorded = null,
            ),
        )

        val state = settled(model)
        assertThat(state.stats.thermalStatus).isEqualTo(ThermalStatus.Nominal)
        assertThat(state.stats.batteryPercent).isNotEqualTo(3)
    }

    @Test
    fun `finishing writes the manifest and marks the project captured`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.Start)
        settled(model)

        model.onEvent(CaptureEvent.Finish)

        val state = settled(model)
        assertThat(state.finishedProjectId).isEqualTo("generated-0")
        assertThat(projects.get("generated-0")?.status).isEqualTo(ProjectStatus.Captured)
        assertThat(storage.readManifest("generated-0")).isNotNull()
        assertThat(playedCues).contains(HapticCue.Success)
    }

    @Test
    fun `the processing estimate is produced from the finished capture`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.Start)
        settled(model)

        model.onEvent(CaptureEvent.Finish)

        // Zero frames gives a zero time estimate, but the storage estimate still has the
        // texture atlas and the volume in it, so it is never zero.
        val state = settled(model)
        assertThat(state.estimatedProcessingBytes).isGreaterThan(0L)
    }

    @Test
    fun `finishing without a session does nothing`() = runTest(mainDispatcher.dispatcher) {
        val model = viewModel()
        settled(model)

        model.onEvent(CaptureEvent.Finish)

        assertThat(settled(model).finishedProjectId).isNull()
    }

    @Test
    fun `an import of nothing is ignored rather than creating an empty project`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)

        model.onEvent(CaptureEvent.ImportMedia(emptyList()))

        val state = settled(model)
        assertThat(state.phase).isEqualTo(CapturePhase.Ready)
        assertThat(state.projectId).isNull()
        assertThat(projects.observeAll().let { true }).isTrue()
    }

    @Test
    fun `an import that is too small reports why and leaves nothing behind`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        // One unreadable URI: fewer than the minimum usable frames.
        model.onEvent(
            CaptureEvent.ImportMedia(listOf(android.net.Uri.parse("content://nothing/1"))),
        )

        val state = settled(model)
        assertThat(state.error).isNotNull()
        assertThat(state.phase).isEqualTo(CapturePhase.Ready)
        assertThat(state.projectId).isNull()
        // An import that failed must not leave a project that cannot be processed.
        assertThat(storage.listProjectIds()).isEmpty()
    }

    @Test
    fun `a session failure is surfaced and can be dismissed`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)

        model.onSessionFailure(
            uk.co.jacoblevy.parallax.feature.capture.session.ArSessionFailure.CameraUnavailable,
        )
        assertThat(settled(model).sessionFailure).isNotNull()

        model.onEvent(CaptureEvent.DismissError)
        assertThat(settled(model).sessionFailure).isNull()
    }

    @Test
    fun `haptics are silent when the user has turned them off`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        settings.setHaptics(false)
        val model = viewModel()
        settled(model)
        playedCues.clear()

        model.onEvent(CaptureEvent.Start)
        settled(model)

        assertThat(playedCues).isEmpty()
    }

    @Test
    fun `finishing needs enough frames to be worth processing`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        model.onEvent(CaptureEvent.Start)
        settled(model)
        assertThat(settled(model).canFinish).isFalse()

        model.onFrameProcessed(
            CaptureTick(
                stats = CaptureStats(keyframeCount = 40),
                coverage = CoverageMap(),
                prompt = null,
                promptChanged = false,
                recorded = null,
            ),
        )

        assertThat(settled(model).canFinish).isTrue()
    }
}
