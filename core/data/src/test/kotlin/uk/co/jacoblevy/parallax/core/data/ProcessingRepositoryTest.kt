package uk.co.jacoblevy.parallax.core.data

import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.core.data.database.ParallaxDatabase
import uk.co.jacoblevy.parallax.core.data.database.ProjectEntity
import uk.co.jacoblevy.parallax.core.data.repository.ProcessingRepository
import uk.co.jacoblevy.parallax.core.data.repository.TimeSource
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ReconstructionStage

@RunWith(RobolectricTestRunner::class)
class ProcessingRepositoryTest {

    private lateinit var database: ParallaxDatabase
    private lateinit var repository: ProcessingRepository
    private var now = 100L

    @Before
    fun setUp() {
        database = Room.inMemoryDatabaseBuilder(
            ApplicationProvider.getApplicationContext(),
            ParallaxDatabase::class.java,
        ).allowMainThreadQueries().build()
        repository = ProcessingRepository(
            dao = database.processingJobDao(),
            time = TimeSource { now },
            io = Dispatchers.Unconfined,
        )
    }

    @After
    fun tearDown() = database.close()

    /** Jobs reference projects by foreign key, so the rows have to exist first. */
    private suspend fun seedProjects(vararg ids: String) {
        for (id in ids) {
            database.projectDao().upsert(
                ProjectEntity(
                    id = id,
                    name = id,
                    captureMode = "Object",
                    createdAtMillis = 0,
                    updatedAtMillis = 0,
                    statusKind = "captured",
                    statusJson = "{\"type\":\"uk.co.jacoblevy.parallax.core.model." +
                        "ProjectStatus.Captured\"}",
                    folderId = null,
                    quality = "Balanced",
                    captureJson = "{}",
                    outputs = "",
                    measurementsJson = null,
                    storageBytes = 0,
                    favourite = false,
                    notes = "",
                ),
            )
        }
    }

    @Test
    fun `queue positions are one-based and in queue order`() = runTest {
        seedProjects("a", "b", "c")
        now = 10
        repository.enqueue("j1", "a", QualityPreset.Fast)
        now = 20
        repository.enqueue("j2", "b", QualityPreset.Fast)
        now = 30
        repository.enqueue("j3", "c", QualityPreset.Fast)

        assertThat(repository.queuePosition("a")).isEqualTo(1)
        assertThat(repository.queuePosition("b")).isEqualTo(2)
        assertThat(repository.queuePosition("c")).isEqualTo(3)
    }

    @Test
    fun `enqueueing the same project twice does not queue it twice`() = runTest {
        seedProjects("a")
        val first = repository.enqueue("j1", "a", QualityPreset.Fast)
        now = 500
        val second = repository.enqueue("j2", "a", QualityPreset.Max)

        assertThat(repository.observeQueue().first()).hasSize(1)
        // The original id and queue time are kept, so a double tap does not send the scan to the
        // back of the queue.
        assertThat(second.id).isEqualTo(first.id)
        assertThat(second.queuedAtMillis).isEqualTo(first.queuedAtMillis)
        assertThat(second.quality).isEqualTo(QualityPreset.Max)
    }

    @Test
    fun `a finished job leaves the queue`() = runTest {
        seedProjects("a")
        repository.enqueue("j1", "a", QualityPreset.Fast)
        repository.markStarted("j1")
        assertThat(repository.observePendingCount().first()).isEqualTo(1)

        repository.markSucceeded("j1")

        assertThat(repository.observePendingCount().first()).isEqualTo(0)
        assertThat(repository.observeQueue().first()).isEmpty()
    }

    @Test
    fun `a failure records its reason and counts the attempt`() = runTest {
        seedProjects("a")
        repository.enqueue("j1", "a", QualityPreset.Balanced)
        repository.markStarted("j1")
        repository.markFailed("j1", "not enough overlap between frames")

        val job = repository.forProject("a")
        assertThat(job?.failureReason).isEqualTo("not enough overlap between frames")
        assertThat(job?.attemptCount).isEqualTo(1)
    }

    @Test
    fun `re-queueing a failed job keeps its progress so it resumes`() = runTest {
        seedProjects("a")
        repository.enqueue("j1", "a", QualityPreset.Balanced)
        repository.markStarted("j1")
        repository.markStageComplete("j1", ReconstructionStage.Depth)
        repository.markFailed("j1", "killed")

        val requeued = repository.enqueue("ignored", "a", QualityPreset.Balanced)

        assertThat(requeued.completedStage).isEqualTo(ReconstructionStage.Depth)
        assertThat(requeued.resumeFrom).isEqualTo(ReconstructionStage.Fusion)
        assertThat(requeued.failureReason).isNull()
        assertThat(requeued.attemptCount).isEqualTo(1)
    }

    @Test
    fun `a fresh job has no completed stage and resumes from the beginning`() = runTest {
        seedProjects("a")
        val job = repository.enqueue("j1", "a", QualityPreset.Fast)
        assertThat(job.completedStage).isNull()
        assertThat(job.resumeFrom).isNull()
        assertThat(job.isRunning).isFalse()
    }

    @Test
    fun `a job orphaned by a process kill is released at startup`() = runTest {
        seedProjects("a", "b")
        repository.enqueue("j1", "a", QualityPreset.Fast)
        repository.enqueue("j2", "b", QualityPreset.Fast)
        repository.markStarted("j1")
        repository.markStageComplete("j1", ReconstructionStage.Fusion)
        assertThat(repository.forProject("a")?.isRunning).isTrue()

        repository.recoverInterrupted()

        val recovered = repository.forProject("a")
        assertThat(recovered?.isRunning).isFalse()
        // The progress it had made survives, so the retry does not start over.
        assertThat(recovered?.completedStage).isEqualTo(ReconstructionStage.Fusion)
        assertThat(repository.observeQueue().first()).hasSize(2)
    }

    @Test
    fun `cancelling removes the job from the queue`() = runTest {
        seedProjects("a")
        repository.enqueue("j1", "a", QualityPreset.Fast)
        repository.cancel("a")
        assertThat(repository.forProject("a")).isNull()
    }

    @Test
    fun `deleting a project takes its job with it`() = runTest {
        seedProjects("a")
        repository.enqueue("j1", "a", QualityPreset.Fast)
        database.projectDao().delete("a")
        assertThat(repository.forProject("a")).isNull()
    }

    @Test
    fun `clearing finished jobs leaves pending ones alone`() = runTest {
        seedProjects("a", "b")
        repository.enqueue("j1", "a", QualityPreset.Fast)
        repository.enqueue("j2", "b", QualityPreset.Fast)
        repository.markSucceeded("j1")

        repository.clearFinished()

        assertThat(repository.forProject("a")).isNull()
        assertThat(repository.forProject("b")).isNotNull()
    }
}
