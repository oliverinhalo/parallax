package uk.co.jacoblevy.parallax.feature.processing

import android.content.Context
import android.util.Log
import androidx.hilt.work.HiltWorker
import androidx.work.CoroutineWorker
import androidx.work.ForegroundInfo
import androidx.work.WorkerParameters
import dagger.assisted.Assisted
import dagger.assisted.AssistedInject
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.data.repository.ProcessingJob
import uk.co.jacoblevy.parallax.core.data.repository.ProcessingRepository
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilityProvider
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.ReconstructionStage

/**
 * Runs the reconstruction queue in a foreground service.
 *
 * One worker drains the whole queue rather than one worker per project. Reconstruction is
 * memory- and thermally-bound, so two running at once is slower than two running in sequence
 * and far more likely to be killed; and a single long-lived foreground notification that says
 * "2 of 4" is what a user expects rather than four competing ones.
 *
 * Everything that survives a kill lives on disk: the queue is in the database and each stage is
 * checkpointed by the engine. A worker that is stopped mid-stage leaves the job marked started,
 * which `recoverInterrupted` clears at the next startup so the queue advances.
 */
@HiltWorker
class ReconstructionWorker @AssistedInject constructor(
    @Assisted private val context: Context,
    @Assisted parameters: WorkerParameters,
    private val projects: ProjectRepository,
    private val processing: ProcessingRepository,
    private val settings: SettingsRepository,
    private val capabilities: DeviceCapabilityProvider,
    private val runner: ReconstructionRunner,
    private val notifications: ProcessingNotifications,
) : CoroutineWorker(context, parameters) {

    override suspend fun getForegroundInfo(): ForegroundInfo =
        notifications.foregroundInfo(
            title = "Preparing to process",
            detail = null,
            progress = 0f,
        )

    override suspend fun doWork(): Result {
        val resolved = capabilities.resolve()
        var processed = 0
        var failed = 0

        while (!isStopped) {
            val job = processing.pending().firstOrNull { it.startedAtMillis == null } ?: break
            when (runOne(job, resolved)) {
                JobResult.Processed -> processed++
                JobResult.Failed -> failed++
                JobResult.Skipped -> Unit
                // Retry rather than failure: the queue still has work, and WorkManager will
                // bring the worker back when its constraints are met again.
                JobResult.Cancelled -> return Result.retry()
            }
        }

        if (isStopped) return Result.retry()
        notifications.clearProgress()
        return if (failed > 0 && processed == 0) Result.failure() else Result.success()
    }

    private enum class JobResult { Processed, Failed, Skipped, Cancelled }

    /** Run one queued scan to completion, or until the worker is stopped. */
    private suspend fun runOne(job: ProcessingJob, capabilities: DeviceCapabilities): JobResult {
        val project = projects.get(job.projectId)
        if (project == null) {
            // The project was deleted while queued. The foreign key removes the job too, so
            // this is belt and braces rather than an expected path.
            processing.cancel(job.projectId)
            return JobResult.Skipped
        }

        val position = processing.queuePosition(job.projectId)
        val queueSize = processing.pending().size
        processing.markStarted(job.id)

        val reporter = StatusReporter(job, project, position, queueSize)
        val outcome = coroutineScope {
            val pump = launch {
                while (isActive) {
                    delay(STATUS_INTERVAL_MILLIS)
                    reporter.publish()
                }
            }
            val result = runner.run(
                project = project,
                capabilities = capabilities,
                shouldCancel = { isStopped },
                onProgress = reporter::record,
            )
            pump.cancelAndJoin()
            reporter.publish()
            result
        }

        return when (outcome) {
            is ReconstructionOutcome.Success -> {
                projects.recordOutputs(
                    id = project.id,
                    outputs = outcome.outputs,
                    measurements = outcome.measurements,
                )
                processing.markSucceeded(job.id)
                // Raw frames are the bulk of a project's size and are only needed to reprocess
                // it, so they go unless the user asked to keep them.
                if (!settings.settings.first().keepRawFramesAfterProcessing) {
                    runCatching { projects.purgeIntermediates(project.id) }
                }
                notifications.notifyFinished(project.name, project.id)
                JobResult.Processed
            }
            is ReconstructionOutcome.Failed -> {
                projects.updateStatus(
                    project.id,
                    ProjectStatus.Failed(
                        stage = outcome.resumableFrom ?: ReconstructionStage.Ingest,
                        reason = outcome.error.message,
                        resumableFrom = outcome.resumableFrom,
                    ),
                )
                processing.markFailed(job.id, outcome.error.message)
                Log.w(TAG, "reconstruction failed: ${outcome.error.message}")
                notifications.notifyFailed(project.name, outcome.error.message)
                JobResult.Failed
            }
            ReconstructionOutcome.Cancelled -> {
                // Left queued and un-started so the next run picks it up; the engine's
                // checkpoints mean it resumes rather than restarting.
                projects.updateStatus(project.id, ProjectStatus.Queued(position))
                JobResult.Cancelled
            }
        }
    }

    /**
     * Holds the latest progress and publishes it on a timer.
     *
     * Progress arrives from the native thread many times a second. Writing the database on
     * every callback would be millions of writes over a long reconstruction, so the newest
     * value is kept and written periodically. The database is what the library, the processing
     * screen and the notification all read, so none of them can disagree -- and the status
     * survives the process dying.
     */
    private inner class StatusReporter(
        private val job: ProcessingJob,
        private val project: Project,
        private val position: Int,
        private val queueSize: Int,
    ) {
        private val latest = MutableStateFlow<ReconstructionProgress?>(null)
        private var lastPublishedStage: ReconstructionStage? = null

        fun record(progress: ReconstructionProgress) {
            latest.value = progress
        }

        suspend fun publish() {
            val current = latest.value ?: return
            runCatching {
                projects.updateStatus(
                    project.id,
                    ProjectStatus.Processing(
                        stage = current.stage,
                        stageProgress = current.stageFraction,
                        overallProgress = current.overallFraction,
                        etaMillis = current.etaMillis,
                    ),
                )
            }
            notifications.update(
                title = notificationTitle(project.name, position, queueSize),
                detail = current.detail,
                progress = current.overallFraction,
            )
            // The completed-stage counter is what lets a retry resume rather than restart, so
            // it is written when the stage changes rather than on every tick.
            val previous = lastPublishedStage
            if (previous != null && current.stage.ordinal > previous.ordinal) {
                runCatching { processing.markStageComplete(job.id, previous) }
            }
            lastPublishedStage = current.stage
        }
    }

    private fun notificationTitle(name: String, position: Int, queueSize: Int): String =
        if (queueSize > 1) "$name · $position of $queueSize" else name

    private companion object {
        const val TAG = "ReconstructionWorker"

        /**
         * How often the status and notification are refreshed.
         *
         * Fast enough that a progress bar looks live, slow enough that a twenty-minute
         * reconstruction does a few thousand database writes rather than a few million.
         */
        const val STATUS_INTERVAL_MILLIS = 400L
    }
}
