package uk.co.jacoblevy.parallax.feature.processing

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.toParallaxError
import uk.co.jacoblevy.parallax.core.data.repository.ProcessingJob
import uk.co.jacoblevy.parallax.core.data.repository.ProcessingRepository
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import javax.inject.Inject

/** One row of the queue: the job and the project it belongs to. */
data class QueueEntry(
    val job: ProcessingJob,
    val project: Project,
    val position: Int,
) {
    val status: ProjectStatus get() = project.status

    val isRunning: Boolean get() = job.isRunning || project.status is ProjectStatus.Processing

    /** What this row is waiting for, in words. */
    val waitingReason: String?
        get() = when {
            isRunning -> null
            job.failureReason != null -> job.failureReason
            position > 1 -> "Waiting for $position in the queue"
            else -> "Starting"
        }
}

data class ProcessingUiState(
    val loading: Boolean = true,
    val queue: List<QueueEntry> = emptyList(),
    val recentlyFinished: List<Project> = emptyList(),
    val failed: List<Project> = emptyList(),
    val error: ParallaxError? = null,
) {
    val isEmpty: Boolean
        get() = !loading && queue.isEmpty() && recentlyFinished.isEmpty() && failed.isEmpty()

    val running: QueueEntry? get() = queue.firstOrNull { it.isRunning }
}

sealed interface ProcessingEvent {
    data class Cancel(val projectId: String) : ProcessingEvent
    data class Retry(val projectId: String) : ProcessingEvent
    data class RetryAtQuality(val projectId: String, val quality: QualityPreset) : ProcessingEvent
    data object DismissError : ProcessingEvent
}

/**
 * The processing queue, as the user sees it.
 *
 * Everything here is derived from the database rather than from the worker: the worker may not
 * even be running -- WorkManager can be waiting for a charger -- and the screen still has to say
 * what is queued and why. That also means a process death between leaving and returning to this
 * screen changes nothing about what it shows.
 */
@HiltViewModel
class ProcessingViewModel @Inject constructor(
    private val projects: ProjectRepository,
    private val processing: ProcessingRepository,
    private val scheduler: ProcessingScheduler,
) : ViewModel() {

    private val local = MutableStateFlow(ProcessingUiState())
    val state: StateFlow<ProcessingUiState> = local.asStateFlow()

    init {
        combine(
            processing.observeQueue(),
            projects.observeAll(),
        ) { jobs, allProjects ->
            val byId = allProjects.associateBy { it.id }
            val entries = jobs.mapIndexedNotNull { index, job ->
                val project = byId[job.projectId] ?: return@mapIndexedNotNull null
                QueueEntry(job = job, project = project, position = index + 1)
            }
            ProcessingUiState(
                loading = false,
                queue = entries,
                // "Recently finished" is anything ready that is not in the queue. The library
                // is the real home for finished scans; this is the tail of the queue's story,
                // so the user can see what the run they were watching produced.
                recentlyFinished = allProjects
                    .filter { it.status is ProjectStatus.Ready }
                    .sortedByDescending { it.updatedAtMillis }
                    .take(RECENT_LIMIT),
                failed = allProjects.filter { it.status is ProjectStatus.Failed },
            )
        }
            .onEach { snapshot ->
                local.update { previous ->
                    snapshot.copy(error = previous.error)
                }
            }
            .launchIn(viewModelScope)

        // The queue may have been left mid-run by a process death, in which case nothing is
        // draining it until something asks.
        viewModelScope.launch { runCatching { scheduler.recoverAfterProcessDeath() } }
    }

    fun onEvent(event: ProcessingEvent) {
        when (event) {
            is ProcessingEvent.Cancel -> guard {
                val project = projects.require(event.projectId)
                scheduler.cancel(project)
            }
            is ProcessingEvent.Retry -> guard {
                scheduler.retry(projects.require(event.projectId))
            }
            is ProcessingEvent.RetryAtQuality -> guard {
                // Re-processing at a different quality has to start over: the checkpoints were
                // written for the old voxel size and texture resolution, and resuming from them
                // would produce a model that is neither one quality nor the other.
                val project = projects.require(event.projectId)
                scheduler.enqueue(project, event.quality)
            }
            ProcessingEvent.DismissError -> local.update { it.copy(error = null) }
        }
    }

    private fun guard(block: suspend () -> Unit) {
        viewModelScope.launch {
            runCatching { block() }.onFailure { failure ->
                local.update { it.copy(error = failure.toParallaxError()) }
            }
        }
    }

    private companion object {
        const val RECENT_LIMIT = 5
    }
}
