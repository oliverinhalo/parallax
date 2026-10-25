package uk.co.jacoblevy.parallax.navigation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.toParallaxError
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.feature.processing.ProcessingScheduler
import javax.inject.Inject

/** Where opening a project should take the user. */
sealed interface ProjectDestination {
    /** The model is ready; show it. */
    data class Viewer(val projectId: String) : ProjectDestination

    /** Not processed yet, and now queued. */
    data object Processing : ProjectDestination

    /** Already queued or already running; show the queue without touching it. */
    data object AlreadyProcessing : ProjectDestination

    /** The capture was never finished, so it is resumed rather than processed. */
    data class ResumeCapture(val projectId: String) : ProjectDestination

    data class Failed(val error: ParallaxError) : ProjectDestination
}

/**
 * Where one tap on a scan goes, for every status there is.
 *
 * Pure, and exhaustive over [ProjectStatus] rather than a couple of conditions and an `else`.
 * The `else` was a bug: it meant "carry on capturing" and it caught `Queued` and `Processing`
 * as well as `Capturing`, so tapping a scan that was being reconstructed opened the camera.
 * That is alarming as well as wrong -- the obvious reading is that the scan is gone and this is
 * a new one -- and it is exactly the kind of mistake a `when` with no `else` cannot make.
 */
internal fun destinationFor(project: Project): ProjectDestination {
    // A finished model wins over everything. Once there is something to look at, looking at it
    // is what the tap means, whatever the status says about splats still refining.
    if (project.hasViewableOutput) return ProjectDestination.Viewer(project.id)

    return when (project.status) {
        // The frames are on disk and the manifest was never written, so carrying on capturing
        // is the only sensible action. This is the one case that opens the camera.
        is ProjectStatus.Capturing -> ProjectDestination.ResumeCapture(project.id)

        // Already in the queue, or already running. Showing the queue is the answer; queueing
        // it again would be a second job over the same directory.
        is ProjectStatus.Queued, is ProjectStatus.Processing ->
            ProjectDestination.AlreadyProcessing

        // Captured and waiting, or stopped partway. All three resume from their last
        // checkpoint rather than starting over, which is what the checkpoints are for.
        is ProjectStatus.Captured, is ProjectStatus.Failed, ProjectStatus.Cancelled ->
            ProjectDestination.Processing

        // Ready, but with nothing the viewer can show: the outputs were deleted from under it,
        // or the run finished without writing one. Processing again is the only way back.
        is ProjectStatus.Ready -> ProjectDestination.Processing
    }
}

/**
 * Decides what tapping a scan in the library does.
 *
 * This lives in `:app` rather than in `:feature:library` on purpose: the decision needs both the
 * project's state and the processing queue, and giving the library a dependency on the
 * processing feature to answer it would couple two features that otherwise have nothing to say
 * to each other.
 *
 * One tap, and the obvious thing happens: a finished scan opens, an unprocessed one starts
 * processing, one already running shows the queue, an abandoned capture resumes. Making the
 * user find a separate "process" button for the scan they just tapped is the kind of thing that
 * makes an app feel like a form.
 */
@HiltViewModel
class ProjectLauncherViewModel @Inject constructor(
    private val projects: ProjectRepository,
    private val scheduler: ProcessingScheduler,
) : ViewModel() {

    private val _destinations = MutableSharedFlow<ProjectDestination>(extraBufferCapacity = 4)
    val destinations: SharedFlow<ProjectDestination> = _destinations.asSharedFlow()

    fun open(projectId: String) {
        viewModelScope.launch {
            val destination = runCatching { resolve(projectId) }
                .getOrElse { failure -> ProjectDestination.Failed(failure.toParallaxError()) }
            _destinations.emit(destination)
        }
    }

    private suspend fun resolve(projectId: String): ProjectDestination {
        val project = projects.require(projectId)
        val destination = destinationFor(project)
        // The only side effect, and it is deliberately not part of the decision: keeping it
        // here is what lets the decision be checked against every status without a scheduler.
        if (destination == ProjectDestination.Processing) scheduler.enqueue(project)
        return destination
    }
}
