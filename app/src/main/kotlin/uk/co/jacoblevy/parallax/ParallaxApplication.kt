package uk.co.jacoblevy.parallax

import android.app.Application
import androidx.hilt.work.HiltWorkerFactory
import androidx.work.Configuration
import dagger.hilt.android.HiltAndroidApp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.common.di.ApplicationScope
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.feature.processing.ProcessingScheduler
import javax.inject.Inject

/**
 * The application.
 *
 * It implements [Configuration.Provider] so that WorkManager initialises with Hilt's worker
 * factory: the reconstruction worker has six injected dependencies, and without this it would
 * be constructed reflectively and fail at runtime rather than at build time.
 *
 * The startup recovery is the other reason this class is not empty. A reconstruction killed
 * mid-stage leaves a job marked started and a project marked processing, and nothing would ever
 * pick either up again. Clearing that at launch is what makes the queue self-healing.
 */
@HiltAndroidApp
class ParallaxApplication : Application(), Configuration.Provider {

    @Inject
    lateinit var workerFactory: HiltWorkerFactory

    @Inject
    lateinit var scheduler: ProcessingScheduler

    @Inject
    lateinit var projects: ProjectRepository

    @Inject
    @ApplicationScope
    lateinit var applicationScope: CoroutineScope

    override val workManagerConfiguration: Configuration
        get() = Configuration.Builder()
            .setWorkerFactory(workerFactory)
            .setMinimumLoggingLevel(android.util.Log.INFO)
            .build()

    override fun onCreate() {
        super.onCreate()
        applicationScope.launch {
            // Both of these touch the database and the filesystem, so they run off the main
            // thread and their failures are swallowed: a startup that crashes because a stale
            // row could not be cleaned up would be far worse than the stale row.
            runCatching { scheduler.recoverAfterProcessDeath() }
            runCatching { projects.reconcile() }
        }
    }
}
