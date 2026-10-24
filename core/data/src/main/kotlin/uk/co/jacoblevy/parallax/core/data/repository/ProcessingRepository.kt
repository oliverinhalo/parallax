package uk.co.jacoblevy.parallax.core.data.repository

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext
import uk.co.jacoblevy.parallax.core.common.di.IoDispatcher
import uk.co.jacoblevy.parallax.core.data.database.ProcessingJobDao
import uk.co.jacoblevy.parallax.core.data.database.ProcessingJobEntity
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ReconstructionStage
import javax.inject.Inject
import javax.inject.Singleton

/** A queued reconstruction, as the processing screen shows it. */
data class ProcessingJob(
    val id: String,
    val projectId: String,
    val quality: QualityPreset,
    val queuedAtMillis: Long,
    val startedAtMillis: Long?,
    val finishedAtMillis: Long?,
    /** The last stage known to have completed, or null if none has. */
    val completedStage: ReconstructionStage?,
    val failureReason: String?,
    val attemptCount: Int,
) {
    val isRunning: Boolean get() = startedAtMillis != null && finishedAtMillis == null

    /**
     * Where a resumed run should start.
     *
     * Null means from the beginning. The pipeline also inspects its own checkpoints, so this is
     * a hint for the UI's "resuming from..." text rather than the authority.
     */
    val resumeFrom: ReconstructionStage?
        get() = completedStage?.let { ReconstructionStage.next(it) }
}

/**
 * The processing queue.
 *
 * It lives in the database rather than in WorkManager's own state because the UI needs to show
 * queue positions, per-stage progress and failure reasons, none of which WorkManager models.
 * WorkManager is what wakes the work up; this is what the work is.
 */
@Singleton
class ProcessingRepository @Inject constructor(
    private val dao: ProcessingJobDao,
    private val time: TimeSource,
    @IoDispatcher private val io: CoroutineDispatcher,
) {
    fun observeQueue(): Flow<List<ProcessingJob>> =
        dao.observeQueue().map { rows -> rows.map { it.toDomain() } }

    fun observePendingCount(): Flow<Int> = dao.observePendingCount()

    suspend fun pending(): List<ProcessingJob> =
        withContext(io) { dao.pending().map { it.toDomain() } }

    suspend fun forProject(projectId: String): ProcessingJob? =
        withContext(io) { dao.forProject(projectId)?.toDomain() }

    /**
     * Add a project to the queue, or re-queue one that failed.
     *
     * The project id is unique in the table, so enqueueing twice replaces the existing row
     * instead of processing the same scan twice -- which a user double-tapping "Process" would
     * otherwise do.
     */
    suspend fun enqueue(jobId: String, projectId: String, quality: QualityPreset): ProcessingJob =
        withContext(io) {
            val existing = dao.forProject(projectId)
            val job = ProcessingJobEntity(
                id = existing?.id ?: jobId,
                projectId = projectId,
                quality = quality.name,
                queuedAtMillis = existing?.queuedAtMillis ?: time.nowMillis(),
                startedAtMillis = null,
                finishedAtMillis = null,
                // A re-queue keeps the completed stage so it resumes rather than restarting.
                completedStage = existing?.completedStage ?: -1,
                failureReason = null,
                attemptCount = existing?.attemptCount ?: 0,
            )
            dao.upsert(job)
            job.toDomain()
        }

    suspend fun queuePosition(projectId: String): Int =
        withContext(io) { dao.queuePosition(projectId) }

    suspend fun markStarted(id: String) = withContext(io) {
        dao.markStarted(id, time.nowMillis())
        dao.incrementAttempts(id)
    }

    suspend fun markStageComplete(id: String, stage: ReconstructionStage) =
        withContext(io) { dao.updateCompletedStage(id, stage.ordinal) }

    suspend fun markSucceeded(id: String) =
        withContext(io) { dao.markFinished(id, time.nowMillis(), failureReason = null) }

    suspend fun markFailed(id: String, reason: String) =
        withContext(io) { dao.markFinished(id, time.nowMillis(), failureReason = reason) }

    suspend fun cancel(projectId: String) = withContext(io) { dao.deleteForProject(projectId) }

    suspend fun clearFinished() = withContext(io) { dao.clearFinished() }

    /** Called once at startup to release jobs orphaned by a process kill. */
    suspend fun recoverInterrupted() = withContext(io) { dao.recoverInterrupted() }

    private fun ProcessingJobEntity.toDomain(): ProcessingJob = ProcessingJob(
        id = id,
        projectId = projectId,
        quality = QualityPreset.entries.firstOrNull { it.name == quality }
            ?: QualityPreset.Balanced,
        queuedAtMillis = queuedAtMillis,
        startedAtMillis = startedAtMillis,
        finishedAtMillis = finishedAtMillis,
        completedStage = ReconstructionStage.entries.getOrNull(completedStage),
        failureReason = failureReason,
        attemptCount = attemptCount,
    )
}
