package uk.co.jacoblevy.parallax.feature.capture.logic

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class ArRenderLoopTest {

    private fun step(
        released: Boolean = false,
        pauseRequested: Boolean = false,
        resumeRequested: Boolean = false,
        sessionResumed: Boolean = false,
    ) = ArRenderLoop.step(released, pauseRequested, resumeRequested, sessionResumed)

    @Test
    fun `a paused session is left alone rather than drawn from`() {
        // The bug. After a pause was applied there was no pending request, and the loop fell
        // through to drawing because the session object still existed. ARCore answers
        // `update()` on a paused session by throwing, which ended the scan.
        assertThat(step(sessionResumed = false)).isEqualTo(ArLoopStep.Idle)
    }

    @Test
    fun `a running session is drawn from`() {
        assertThat(step(sessionResumed = true)).isEqualTo(ArLoopStep.Draw)
    }

    @Test
    fun `a pause is applied even while the session is running`() {
        assertThat(step(pauseRequested = true, sessionResumed = true))
            .isEqualTo(ArLoopStep.Pause)
    }

    @Test
    fun `a resume is applied to a session that is not running`() {
        assertThat(step(resumeRequested = true)).isEqualTo(ArLoopStep.Resume)
    }

    @Test
    fun `releasing outranks every other request`() {
        // A session left open is one ARCore still counts, and the next scan meets its second.
        assertThat(
            step(
                released = true,
                pauseRequested = true,
                resumeRequested = true,
                sessionResumed = true,
            ),
        ).isEqualTo(ArLoopStep.Close)
    }

    @Test
    fun `a pause outranks a resume, so a screen going away wins over one coming back`() {
        assertThat(step(pauseRequested = true, resumeRequested = true))
            .isEqualTo(ArLoopStep.Pause)
    }
}
