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
import uk.co.jacoblevy.parallax.feature.processing.ProcessingScheduler
import javax.inject.Inject

/** Where opening a project should take the user. */
sealed interface ProjectDestination {
    /** The model is ready; show it. */
    data class Viewer(val projectId: String) : ProjectDestination

    /** The scan is captured but not processed, and has just been queued. */
    data object Processing : ProjectDestination

    /** The capture was never finished, so it is resumed rather than processed. */
    data class ResumeCapture(val projectId: String) : ProjectDestination

    data class Failed(val error: ParallaxError) : ProjectDestination
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
 * processing, an abandoned capture resumes. Making the user find a separate "process" button for
 * the scan they just tapped is the kind of thing that makes an app feel like a form.
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
        return when {
            project.hasViewableOutput -> ProjectDestination.Viewer(projectId)
            project.canProcess -> {
                scheduler.enqueue(project)
                ProjectDestination.Processing
            }
            // Capturing: the frames are on disk and the manifest was never written, so the
            // only sensible action is to carry on capturing.
            else -> ProjectDestination.ResumeCapture(projectId)
        }
    }
}
