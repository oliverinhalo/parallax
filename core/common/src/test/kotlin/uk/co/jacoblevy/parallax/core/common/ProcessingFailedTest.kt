package uk.co.jacoblevy.parallax.core.common

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.RecoveryAction

/**
 * What a reconstruction failure tells the user.
 *
 * The engine's stage messages are written to be read by whoever is holding the phone -- "the
 * depth maps and camera poses may disagree", not a code -- and they were carried from native
 * code, through the worker, into this class, and then dropped. Every reconstruction failure
 * read the same: a stage name and a retry button.
 */
class ProcessingFailedTest {

    @Test
    fun `the reason the stage gave is part of what the user is told`() {
        val error = ParallaxError.ProcessingFailed(
            stage = "Fusing surfaces",
            detail = "none of the 18 keyframes had a depth map, so there was nothing to fuse",
            resumableFromStage = "Fusion",
        )

        assertThat(error.message).contains("Fusing surfaces")
        assertThat(error.message).contains("none of the 18 keyframes had a depth map")
    }

    @Test
    fun `a stage that failed without saying why still reads as a sentence`() {
        val error = ParallaxError.ProcessingFailed(
            stage = "Fusing surfaces",
            detail = "   ",
            resumableFromStage = null,
        )

        assertThat(error.message).isEqualTo("Processing stopped during Fusing surfaces.")
    }

    @Test
    fun `a checkpointed failure offers to resume rather than to start again`() {
        val error = ParallaxError.ProcessingFailed(
            stage = "Fusing surfaces",
            detail = "boom",
            resumableFromStage = "Fusion",
        )

        assertThat(error.recovery).isEqualTo(RecoveryAction.ResumeFrom("Fusion"))
        assertThat(error.retryable).isTrue()
    }
}
