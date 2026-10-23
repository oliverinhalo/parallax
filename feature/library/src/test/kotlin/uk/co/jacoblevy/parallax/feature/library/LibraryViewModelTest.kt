package uk.co.jacoblevy.parallax.feature.library

import android.net.Uri
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import uk.co.jacoblevy.parallax.core.testing.DirectExecutor
import uk.co.jacoblevy.parallax.core.testing.MainDispatcherRule
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.data.database.ParallaxDatabase
import uk.co.jacoblevy.parallax.core.data.repository.FolderRepository
import uk.co.jacoblevy.parallax.core.data.repository.IdGenerator
import uk.co.jacoblevy.parallax.core.data.repository.ProcessingRepository
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.repository.TimeSource
import uk.co.jacoblevy.parallax.core.data.storage.ProjectBundle
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.data.storage.ProjectTransfer
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.LibrarySort
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import java.io.File

@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
class LibraryViewModelTest {

    @get:Rule
    val mainDispatcher = MainDispatcherRule()

    private lateinit var database: ParallaxDatabase
    private lateinit var storage: ProjectStorage
    private lateinit var projects: ProjectRepository
    private lateinit var folders: FolderRepository
    private lateinit var processing: ProcessingRepository
    private lateinit var transfer: ProjectTransfer
    private val ids = IdGenerator { "generated-${nextId++}" }
    private var now = 1_000L
    private var nextId = 0

    @Before
    fun setUp() {
        val context = ApplicationProvider.getApplicationContext<android.content.Context>()
        database = Room.inMemoryDatabaseBuilder(context, ParallaxDatabase::class.java)
            // Direct executors keep a write and the Flow emission it causes on this thread, so
            // the state is settled by the time the next assertion runs.
            .setQueryExecutor(DirectExecutor)
            .setTransactionExecutor(DirectExecutor)
            .allowMainThreadQueries()
            .build()
        storage = ProjectStorage(context)
        val time = TimeSource { now }
        projects = ProjectRepository(
            projectDao = database.projectDao(),
            annotationDao = database.annotationDao(),
            measurementDao = database.measurementDao(),
            storage = storage,
            bundles = ProjectBundle(storage),
            time = time,
            io = Dispatchers.Unconfined,
        )
        folders = FolderRepository(database.folderDao(), time, Dispatchers.Unconfined)
        processing = ProcessingRepository(
            database.processingJobDao(),
            time,
            Dispatchers.Unconfined,
        )
        transfer = ProjectTransfer(
            context = context,
            projects = projects,
            ids = ids,
            storage = storage,
            io = Dispatchers.Unconfined,
        )
    }

    @After
    fun tearDown() {
        database.close()
        storage.root.deleteRecursively()
    }

    private fun viewModel() = LibraryViewModel(
        projects = projects,
        folders = folders,
        processing = processing,
        storage = storage,
        transfer = transfer,
        ids = ids,
    )

    private suspend fun seed(id: String, name: String, mode: CaptureMode = CaptureMode.Object) =
        projects.create(id, name, mode, QualityPreset.Balanced)

    /** Run everything the ViewModel has queued, then read the settled state. */
    private fun settled(model: LibraryViewModel): LibraryUiState {
        mainDispatcher.scheduler.advanceUntilIdle()
        return model.state.value
    }

    @Test
    fun `an empty library is reported as empty rather than as filtered`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val state = settled(viewModel())
        assertThat(state.loading).isFalse()
        assertThat(state.isEmpty).isTrue()
        assertThat(state.isFilteredEmpty).isFalse()
    }

    @Test
    fun `a filter that hides everything is distinguished from an empty library`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        seed("p1", "Chair")
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.ModeFilterChanged(CaptureMode.Room))

        val state = settled(model)
        assertThat(state.projects).isEmpty()
        assertThat(state.isFilteredEmpty).isTrue()
        assertThat(state.isEmpty).isFalse()
    }

    @Test
    fun `clearing filters keeps the sort order the user chose`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        model.onEvent(LibraryEvent.SortChanged(LibrarySort.LargestFirst))
        model.onEvent(LibraryEvent.ToggleFavouritesOnly)
        model.onEvent(LibraryEvent.ClearFilters)

        val state = settled(model)
        assertThat(state.query.sort).isEqualTo(LibrarySort.LargestFirst)
        assertThat(state.query.isFiltered).isFalse()
    }

    @Test
    fun `search finds a scan by name and by note`() = runTest(mainDispatcher.dispatcher) {
        seed("p1", "Kitchen worktop")
        seed("p2", "Garden bench")
        projects.setNotes("p1", "the one by the window")
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.SearchChanged("bench"))
        assertThat(settled(model).projects.single().id).isEqualTo("p2")

        model.onEvent(LibraryEvent.SearchChanged("WINDOW"))
        assertThat(settled(model).projects.single().id).isEqualTo("p1")
    }

    @Test
    fun `selection mode starts on the first selection and ends when cleared`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        seed("p1", "A")
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.ToggleSelected("p1"))
        assertThat(settled(model).selection).containsExactly("p1")

        model.onEvent(LibraryEvent.ToggleSelected("p1"))
        assertThat(settled(model).inSelectionMode).isFalse()
    }

    @Test
    fun `deleting asks first and only then removes the scans`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        seed("p1", "A")
        seed("p2", "B")
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.RequestDelete(setOf("p1", "p2")))
        assertThat(settled(model).pendingDelete).hasSize(2)
        // Nothing has gone yet: a destructive action behind a single tap is how libraries of
        // irreplaceable local data get lost.
        assertThat(projects.get("p1")).isNotNull()

        model.onEvent(LibraryEvent.ConfirmDelete)
        settled(model)

        assertThat(projects.get("p1")).isNull()
        assertThat(projects.get("p2")).isNull()
        assertThat(storage.exists("p1")).isFalse()
    }

    @Test
    fun `cancelling a delete keeps everything`() = runTest(mainDispatcher.dispatcher) {
        seed("p1", "A")
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.RequestDelete(setOf("p1")))
        model.onEvent(LibraryEvent.CancelDelete)

        assertThat(settled(model).pendingDelete).isEmpty()
        assertThat(projects.get("p1")).isNotNull()
    }

    @Test
    fun `a delete confirms how much space it freed`() = runTest(mainDispatcher.dispatcher) {
        seed("p1", "A")
        storage.outputsDir("p1").resolve("model.glb").writeBytes(ByteArray(4_096))
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.RequestDelete(setOf("p1")))
        model.onEvent(LibraryEvent.ConfirmDelete)

        assertThat(settled(model).notice).contains("4.0 KB")
    }

    @Test
    fun `renaming applies the new name and closes the prompt`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val project = seed("p1", "Untitled")
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.RequestRename(project))
        assertThat(settled(model).renaming).isNotNull()

        model.onEvent(LibraryEvent.ConfirmRename("Garden bench"))

        val state = settled(model)
        assertThat(state.projects.single().name).isEqualTo("Garden bench")
        assertThat(state.renaming).isNull()
    }

    @Test
    fun `a blank rename falls back to a usable name rather than an empty one`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val project = seed("p1", "Untitled")
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.RequestRename(project))
        model.onEvent(LibraryEvent.ConfirmRename("   "))

        assertThat(settled(model).projects.single().name).isEqualTo("Untitled scan")
    }

    @Test
    fun `favourites can be toggled from the grid`() = runTest(mainDispatcher.dispatcher) {
        seed("p1", "A")
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.ToggleFavourite("p1"))
        assertThat(settled(model).projects.single().favourite).isTrue()

        model.onEvent(LibraryEvent.ToggleFavourite("p1"))
        assertThat(settled(model).projects.single().favourite).isFalse()
    }

    @Test
    fun `creating a folder uses a generated id and shows up with a zero count`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)

        model.onEvent(LibraryEvent.ConfirmCreateFolder("Furniture"))

        val state = settled(model)
        assertThat(state.folders.single().id).isEqualTo("generated-0")
        assertThat(state.folders.single().projectCount).isEqualTo(0)
        assertThat(state.creatingFolder).isFalse()
    }

    @Test
    fun `deleting the folder currently being filtered on clears the filter`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        folders.create("f1", "Furniture")
        seed("p1", "A")
        projects.moveToFolder("p1", "f1")
        val model = viewModel()
        settled(model)
        model.onEvent(LibraryEvent.FolderSelected("f1"))
        assertThat(settled(model).projects).hasSize(1)

        model.onEvent(LibraryEvent.DeleteFolder("f1"))

        // Without clearing the filter the library would look empty, which reads as data loss.
        val state = settled(model)
        assertThat(state.query.folderId).isNull()
        assertThat(state.projects.map { it.id }).containsExactly("p1")
    }

    @Test
    fun `moving scans to a folder clears the selection`() = runTest(mainDispatcher.dispatcher) {
        folders.create("f1", "Furniture")
        seed("p1", "A")
        seed("p2", "B")
        val model = viewModel()
        settled(model)
        model.onEvent(LibraryEvent.ToggleSelected("p1"))
        model.onEvent(LibraryEvent.ToggleSelected("p2"))

        model.onEvent(LibraryEvent.RequestMove(setOf("p1", "p2")))
        model.onEvent(LibraryEvent.ConfirmMove("f1"))

        val state = settled(model)
        assertThat(state.selection).isEmpty()
        assertThat(state.projects.all { it.folderId == "f1" }).isTrue()
        assertThat(state.folders.single().projectCount).isEqualTo(2)
    }

    @Test
    fun `a deleted scan leaves the selection it was part of`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        seed("p1", "A")
        seed("p2", "B")
        val model = viewModel()
        settled(model)
        model.onEvent(LibraryEvent.ToggleSelected("p1"))
        model.onEvent(LibraryEvent.ToggleSelected("p2"))

        // As if it went away from under the user -- a finished background job, or another screen.
        projects.delete("p1")

        // Otherwise the action bar would offer to delete something that is already gone.
        assertThat(settled(model).selection).containsExactly("p2")
    }

    @Test
    fun `the queue count is surfaced so the library can link to it`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        seed("p1", "A")
        processing.enqueue("j1", "p1", QualityPreset.Fast)

        assertThat(settled(viewModel()).queuedCount).isEqualTo(1)
    }

    @Test
    fun `a failed action surfaces an actionable error rather than failing silently`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val model = viewModel()
        settled(model)
        model.onEvent(LibraryEvent.ConfirmCreateFolder("Furniture"))
        assertThat(settled(model).folders).hasSize(1)

        // Folder names are unique in the schema, which makes this a realistic failure rather
        // than a contrived one.
        model.onEvent(LibraryEvent.ConfirmCreateFolder("Furniture"))

        val failed = settled(model)
        assertThat(failed.error).isNotNull()
        assertThat(failed.error?.recovery).isNotNull()

        model.onEvent(LibraryEvent.DismissError)
        assertThat(settled(model).error).isNull()
    }

    @Test
    fun `startup reconciliation reclaims a directory with no row`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        storage.create("ghost")
        storage.outputsDir("ghost").resolve("x").writeBytes(ByteArray(64))

        settled(viewModel())

        assertThat(storage.exists("ghost")).isFalse()
    }

    @Test
    fun `importing a bundle adds a second project and confirms it by name`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        seed("p1", "Garden bench")
        storage.outputsDir("p1").resolve("model.glb").writeBytes(ByteArray(128))
        val bundle = File.createTempFile("bundle", ".parallax")
        bundle.outputStream().use { projects.exportBundle("p1", it, includeRawFrames = false) }

        val model = viewModel()
        settled(model)
        model.onEvent(LibraryEvent.ImportBundle(Uri.fromFile(bundle)))

        val state = settled(model)
        assertThat(state.notice).contains("Garden bench")
        assertThat(state.projects).hasSize(2)
        // The copy is a new project, not a second row pointing at the same files.
        assertThat(state.projects.map { it.id }).contains("generated-0")
        assertThat(state.importing).isFalse()
        bundle.delete()
    }

    @Test
    fun `a bundle that is not a bundle reports why instead of failing silently`() = runTest(
        mainDispatcher.dispatcher,
    ) {
        val notABundle = File.createTempFile("random", ".parallax")
        notABundle.writeText("this is not a zip")

        val model = viewModel()
        settled(model)
        model.onEvent(LibraryEvent.ImportBundle(Uri.fromFile(notABundle)))

        val state = settled(model)
        assertThat(state.error).isNotNull()
        assertThat(state.importing).isFalse()
        assertThat(state.projects).isEmpty()
        notABundle.delete()
    }
}
