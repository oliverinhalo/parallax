package uk.co.jacoblevy.parallax.feature.processing

import android.content.Context
import androidx.work.Constraints
import androidx.work.ExistingWorkPolicy
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkManager
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.first
import uk.co.jacoblevy.parallax.core.data.repository.IdGenerator
import uk.co.jacoblevy.parallax.core.data.repository.ProcessingRepository
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.ParallaxException
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Puts scans into the queue and makes sure something is draining it.
 *
 * The queue itself lives in the database (see `ProcessingRepository`), not in WorkManager:
 * queue positions, per-stage progress and failure reasons are all things the UI shows and
 * WorkManager models none of them. WorkManager's job here is narrower -- survive the app being
 * backgrounded, respect the charging constraint, and restart the drain after a process death.
 */
@Singleton
class ProcessingScheduler @Inject constructor(
    @ApplicationContext private val context: Context,
    private val projects: ProjectRepository,
    private val processing: ProcessingRepository,
    private val settings: SettingsRepository,
    private val storage: ProjectStorage,
    private val ids: IdGenerator,
) {
    private val workManager: WorkManager get() = WorkManager.getInstance(context)

    /**
     * Queue a project for reconstruction.
     *
     * @throws ParallaxException when the capture is not usable, which is checked here rather
     *   than when the job reaches the front of the queue: finding out twenty minutes later that
     *   a scan was never processable is a much worse experience.
     */
    suspend fun enqueue(project: Project, quality: QualityPreset = project.quality) {
        if (!storage.hasUsableCapture(project.id)) {
            throw ParallaxException(
                ParallaxError.CaptureTooShort(
                    frameCount = project.capture.keyframeCount,
                    minimumFrames = ProjectStorage.MIN_FRAMES,
                ),
            )
        }
        if (quality != project.quality) {
            projects.upsert(project.copy(quality = quality))
        }
        processing.enqueue(ids.newId(), project.id, quality)
        projects.updateStatus(
            project.id,
            ProjectStatus.Queued(processing.queuePosition(project.id)),
        )
        start()
    }

    /** Re-queue a failed scan. It resumes from its last completed stage. */
    suspend fun retry(project: Project) = enqueue(project)

    /**
     * Remove a scan from the queue.
     *
     * The work request is not cancelled: one worker drains the whole queue, so cancelling it
     * would stop the other scans too. The worker notices the row is gone on its next pass.
     */
    suspend fun cancel(project: Project) {
        processing.cancel(project.id)
        projects.updateStatus(project.id, ProjectStatus.Captured)
    }

    /**
     * Make sure a worker is running.
     *
     * `KEEP` rather than `REPLACE`: a worker already draining the queue will pick up anything
     * newly added, and replacing it would discard the stage it is part-way through.
     */
    suspend fun start() {
        val stored = settings.settings.first()
        val constraints = Constraints.Builder()
            // Reconstruction is heavy enough to matter to a nearly flat battery, and the user
            // can ask for it to wait for a charger.
            .setRequiresBatteryNotLow(true)
            .apply { if (stored.processOnlyWhileCharging) setRequiresCharging(true) }
            .setRequiresStorageNotLow(true)
            .build()

        // Deliberately not expedited. WorkManager rejects an expedited request that carries
        // battery or charging constraints outright -- it only allows network and storage ones --
        // and expedited work has a short quota that a multi-minute reconstruction would blow
        // through anyway. The worker promotes itself to a foreground service instead, which is
        // the right mechanism for work the user is watching.
        val request = OneTimeWorkRequestBuilder<ReconstructionWorker>()
            .setConstraints(constraints)
            .addTag(TAG)
            .build()

        workManager.enqueueUniqueWork(UNIQUE_WORK, ExistingWorkPolicy.KEEP, request)
    }

    /**
     * Recover the queue at startup.
     *
     * A worker killed mid-stage leaves its job marked started, and nothing would ever pick it
     * up again. Clearing that flag is what makes the queue self-healing; the engine's
     * checkpoints mean the retry resumes rather than restarting.
     */
    suspend fun recoverAfterProcessDeath() {
        processing.recoverInterrupted()
        if (processing.pending().isNotEmpty()) start()
    }

    private companion object {
        const val UNIQUE_WORK = "parallax.reconstruction"
        const val TAG = "reconstruction"
    }
}
