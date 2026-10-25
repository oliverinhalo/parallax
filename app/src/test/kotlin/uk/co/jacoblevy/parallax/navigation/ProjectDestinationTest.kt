package uk.co.jacoblevy.parallax.navigation

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.ReconstructionStage

/**
 * One tap on a scan.
 *
 * Every status is checked, because the bug was a status nobody had thought about falling into
 * an `else` that opened the camera. A scan being reconstructed looked, to the person who tapped
 * it, like a scan that had been thrown away.
 */
class ProjectDestinationTest {

    private fun project(
        status: ProjectStatus,
        outputs: Set<OutputKind> = emptySet(),
    ) = Project(
        id = "p1",
        name = "Kitchen",
        captureMode = CaptureMode.Room,
        createdAtMillis = 0,
        updatedAtMillis = 0,
        status = status,
        outputs = outputs,
    )

    @Test
    fun `a scan being reconstructed shows the queue, not the camera`() {
        val destination = destinationFor(
            project(
                ProjectStatus.Processing(
                    stage = ReconstructionStage.Fusion,
                    stageProgress = 0.5f,
                    overallProgress = 0.4f,
                ),
            ),
        )

        assertThat(destination).isEqualTo(ProjectDestination.AlreadyProcessing)
    }

    @Test
    fun `a scan waiting in the queue shows the queue, and is not queued twice`() {
        val destination = destinationFor(project(ProjectStatus.Queued(position = 2)))

        assertThat(destination).isEqualTo(ProjectDestination.AlreadyProcessing)
    }

    @Test
    fun `an unfinished capture is the only thing that opens the camera`() {
        val destination = destinationFor(project(ProjectStatus.Capturing(frameCount = 14)))

        assertThat(destination).isEqualTo(ProjectDestination.ResumeCapture("p1"))
    }

    @Test
    fun `a captured scan is queued`() {
        assertThat(destinationFor(project(ProjectStatus.Captured)))
            .isEqualTo(ProjectDestination.Processing)
    }

    @Test
    fun `a failed scan is queued again, to resume from its checkpoint`() {
        val destination = destinationFor(
            project(
                ProjectStatus.Failed(
                    stage = ReconstructionStage.Fusion,
                    reason = "no surface was found",
                    resumableFrom = ReconstructionStage.Fusion,
                ),
            ),
        )

        assertThat(destination).isEqualTo(ProjectDestination.Processing)
    }

    @Test
    fun `a cancelled scan is queued again rather than recaptured`() {
        assertThat(destinationFor(project(ProjectStatus.Cancelled)))
            .isEqualTo(ProjectDestination.Processing)
    }

    @Test
    fun `anything with a model to show opens the viewer`() {
        val destination = destinationFor(
            project(ProjectStatus.Ready(), outputs = setOf(OutputKind.Mesh)),
        )

        assertThat(destination).isEqualTo(ProjectDestination.Viewer("p1"))
    }

    @Test
    fun `splats still refining do not stop the model being opened`() {
        val destination = destinationFor(
            project(
                ProjectStatus.Ready(refiningSplats = true),
                outputs = setOf(OutputKind.Mesh),
            ),
        )

        assertThat(destination).isEqualTo(ProjectDestination.Viewer("p1"))
    }

    @Test
    fun `ready with nothing to show is processed again rather than opening an empty viewer`() {
        assertThat(destinationFor(project(ProjectStatus.Ready())))
            .isEqualTo(ProjectDestination.Processing)
    }
}
