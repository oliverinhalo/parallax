package uk.co.jacoblevy.parallax.feature.settings

import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.data.database.ParallaxDatabase
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.repository.TimeSource
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.data.storage.ProjectBundle
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilityProvider
import uk.co.jacoblevy.parallax.core.model.LengthUnit
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ThemeMode
import uk.co.jacoblevy.parallax.core.testing.DirectExecutor
import uk.co.jacoblevy.parallax.core.testing.MainDispatcherRule
import java.io.File

@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
class SettingsViewModelTest {

    @get:Rule
    val mainDispatcher = MainDispatcherRule()

    private lateinit var database: ParallaxDatabase
    private lateinit var storage: ProjectStorage
    private lateinit var projects: ProjectRepository
    private lateinit var settings: SettingsRepository
    private var capabilities = DeviceCapabilities(
        totalRamBytes = 8L * 1024 * 1024 * 1024,
        supportsVulkanCompute = true,
        cpuCoreCount = 8,
    )

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
        val dir = File(context.filesDir, "settings-test").apply { mkdirs() }
        File(dir, "settings.preferences_pb").delete()
        settings = SettingsRepository(
            // DataStore is given the test dispatcher's scope. With its default
            // (Dispatchers.IO) a write lands on a real background thread, so
            // `advanceUntilIdle()` returns before the setting has been stored and the
            // assertion reads the old value about half the time.
            PreferenceDataStoreFactory.create(
                scope = CoroutineScope(mainDispatcher.dispatcher + Job()),
            ) { File(dir, "settings.preferences_pb") },
        )
    }

    @After
    fun tearDown() {
        database.close()
        storage.root.deleteRecursively()
    }

    private fun viewModel() = SettingsViewModel(
        settings = settings,
        projects = projects,
        storage = storage,
        capabilities = object : DeviceCapabilityProvider {
            override suspend fun resolve(): DeviceCapabilities = capabilities
        },
    )

    private fun settled(model: SettingsViewModel): SettingsUiState {
        mainDispatcher.scheduler.advanceUntilIdle()
        return model.state.value
    }

    /** A finished scan that still has its frames on disk, i.e. one that can be reclaimed. */
    private suspend fun finishedScanWithFrames(id: String, frameBytes: Int) {
        projects.create(id, id, CaptureMode.Object, QualityPreset.Balanced)
        storage.framesDir(id).resolve("000001.jpg").writeBytes(ByteArray(frameBytes))
        storage.outputsDir(id).resolve("model.glb").writeBytes(ByteArray(512))
        projects.recordOutputs(id, setOf(OutputKind.Mesh), measurements = null)
    }

    @Test
    fun `settings changes are persisted and reflected in the state`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)

        model.onEvent(SettingsEvent.SetTheme(ThemeMode.Light))
        model.onEvent(SettingsEvent.SetUnits(LengthUnit.Feet))
        model.onEvent(SettingsEvent.SetDefaultQuality(QualityPreset.Max))

        val state = settled(model)
        assertThat(state.settings.theme).isEqualTo(ThemeMode.Light)
        assertThat(state.settings.units).isEqualTo(LengthUnit.Feet)
        assertThat(state.settings.defaultQuality).isEqualTo(QualityPreset.Max)
        // And they survive a new ViewModel, i.e. they really went to disk.
        assertThat(settled(viewModel()).settings.units).isEqualTo(LengthUnit.Feet)
    }

    @Test
    fun `the storage breakdown is read from disk`() = runTest(mainDispatcher.dispatcher) {
        finishedScanWithFrames("p1", frameBytes = 4_096)

        val state = settled(viewModel())

        assertThat(state.projectCount).isEqualTo(1)
        assertThat(state.storage.rawFramesBytes).isAtLeast(4_096L)
        assertThat(state.storage.reclaimableBytes).isAtLeast(4_096L)
        assertThat(state.reclaimableProjects).isEqualTo(1)
    }

    @Test
    fun `an unprocessed capture is never offered for reclamation`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        // Its frames are the only copy of data no model has been built from yet.
        projects.create("p1", "A", CaptureMode.Object, QualityPreset.Balanced)
        storage.framesDir("p1").resolve("000001.jpg").writeBytes(ByteArray(4_096))

        assertThat(settled(viewModel()).reclaimableProjects).isEqualTo(0)
    }

    @Test
    fun `purging asks first, then frees the frames and keeps the model`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        finishedScanWithFrames("p1", frameBytes = 8_192)
        val model = viewModel()
        settled(model)

        model.onEvent(SettingsEvent.RequestPurgeIntermediates)
        assertThat(settled(model).confirmingPurge).isTrue()
        assertThat(storage.framesDir("p1").listFiles()).isNotEmpty()

        model.onEvent(SettingsEvent.ConfirmPurgeIntermediates)

        val state = settled(model)
        assertThat(state.notice).contains("8.0 KB")
        assertThat(state.busy).isFalse()
        assertThat(storage.framesDir("p1").listFiles()).isEmpty()
        assertThat(storage.hasModel("p1")).isTrue()
        assertThat(state.storage.rawFramesBytes).isEqualTo(0)
    }

    @Test
    fun `cancelling a purge changes nothing`() = runTest(mainDispatcher.dispatcher) {
        finishedScanWithFrames("p1", frameBytes = 2_048)
        val model = viewModel()
        settled(model)

        model.onEvent(SettingsEvent.RequestPurgeIntermediates)
        model.onEvent(SettingsEvent.CancelPurgeIntermediates)

        assertThat(settled(model).confirmingPurge).isFalse()
        assertThat(storage.framesDir("p1").listFiles()).isNotEmpty()
    }

    @Test
    fun `resetting restores defaults but keeps the onboarding flag`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        settings.setOnboardingComplete(true)
        model.onEvent(SettingsEvent.SetUnits(LengthUnit.Inches))
        model.onEvent(SettingsEvent.SetTheme(ThemeMode.Light))

        model.onEvent(SettingsEvent.RequestReset)
        model.onEvent(SettingsEvent.ConfirmReset)

        val state = settled(model)
        assertThat(state.settings.units).isEqualTo(LengthUnit.Metres)
        assertThat(state.settings.theme).isEqualTo(ThemeMode.Dark)
        // Showing the tour again to someone who finished it would read as a bug.
        assertThat(state.settings.onboardingComplete).isTrue()
        assertThat(state.notice).isNotNull()
    }

    @Test
    fun `device limitations are surfaced rather than features silently vanishing`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        capabilities = DeviceCapabilities(
            totalRamBytes = 3L * 1024 * 1024 * 1024,
            supportsVulkanCompute = false,
        )

        val state = settled(viewModel())

        assertThat(state.capabilities.canTrainSplats).isFalse()
        assertThat(state.capabilities.limitations).isNotEmpty()
        assertThat(state.capabilities.maxQualityPreset).isEqualTo(QualityPreset.Fast)
    }

    @Test
    fun `the engine reports itself as unavailable on the JVM rather than crashing`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        // There is no .so on a JVM test host. The bridge has to degrade to a reason string;
        // throwing here would take the settings screen down on any device with a bad install.
        val state = settled(viewModel())
        assertThat(state.engineUnavailableReason).isNotNull()
    }
}
