package uk.co.jacoblevy.parallax.core.common

import com.google.common.truth.Truth.assertThat
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.test.runTest
import org.junit.Test
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.ParallaxException
import uk.co.jacoblevy.parallax.core.common.result.ParallaxResult
import uk.co.jacoblevy.parallax.core.common.result.RecoveryAction
import uk.co.jacoblevy.parallax.core.common.result.asParallaxResult
import uk.co.jacoblevy.parallax.core.common.result.flatMap
import uk.co.jacoblevy.parallax.core.common.result.getOrElse
import uk.co.jacoblevy.parallax.core.common.result.map
import uk.co.jacoblevy.parallax.core.common.result.onFailure
import uk.co.jacoblevy.parallax.core.common.result.onSuccess
import uk.co.jacoblevy.parallax.core.common.result.runCatchingParallax
import uk.co.jacoblevy.parallax.core.common.result.toParallaxError

class ParallaxResultTest {

    private val error = ParallaxError.ProjectNotFound("p1")

    @Test
    fun `map and flatMap only run on success`() {
        assertThat(ParallaxResult.Success(2).map { it * 3 }.getOrNull()).isEqualTo(6)
        assertThat(ParallaxResult.Failure(error).map { 1 }.errorOrNull()).isEqualTo(error)
        assertThat(ParallaxResult.Loading.map { 1 }.isLoading).isTrue()

        val chained = ParallaxResult.Success(2).flatMap { ParallaxResult.Success(it + 1) }
        assertThat(chained.getOrNull()).isEqualTo(3)
    }

    @Test
    fun `side-effect helpers fire for the matching case only`() {
        var successes = 0
        var failures = 0
        ParallaxResult.Success(1).onSuccess { successes++ }.onFailure { failures++ }
        ParallaxResult.Failure(error).onSuccess { successes++ }.onFailure { failures++ }
        assertThat(successes).isEqualTo(1)
        assertThat(failures).isEqualTo(1)
    }

    @Test
    fun `getOrElse supplies a fallback for failure and for loading`() {
        assertThat(ParallaxResult.Success(5).getOrElse { 0 }).isEqualTo(5)
        assertThat(ParallaxResult.Failure(error).getOrElse { 0 }).isEqualTo(0)
        assertThat(ParallaxResult.Loading.getOrElse { 0 }).isEqualTo(0)
    }

    @Test
    fun `runCatchingParallax converts a throwable into an actionable error`() {
        val result = runCatchingParallax { error("disk is gone") }
        assertThat(result.isFailure).isTrue()
        assertThat(result.errorOrNull()).isInstanceOf(ParallaxError.Unknown::class.java)
    }

    @Test
    fun `runCatchingParallax lets cancellation through`() {
        // Swallowing cancellation would break structured concurrency: a cancelled
        // reconstruction would carry on running in the background.
        val thrown = runCatching {
            runCatchingParallax { throw CancellationException("stop") }
        }.exceptionOrNull()
        assertThat(thrown).isInstanceOf(CancellationException::class.java)
    }

    @Test
    fun `a flow emits loading first and converts a throw into a failure`() = runTest {
        val emissions = flow {
            emit(1)
            throw IllegalStateException("boom")
        }.asParallaxResult().toList()

        assertThat(emissions[0].isLoading).isTrue()
        assertThat(emissions[1].getOrNull()).isEqualTo(1)
        assertThat(emissions[2].isFailure).isTrue()
    }

    @Test
    fun `every error kind carries a user-facing message`() {
        val errors = listOf(
            ParallaxError.InsufficientStorage(requiredBytes = 10, availableBytes = 1),
            ParallaxError.CaptureTooShort(frameCount = 1, minimumFrames = 20),
            ParallaxError.TrackingLost(recoveredFrames = 0),
            ParallaxError.ReconstructionEmpty(stage = "Fusion"),
            ParallaxError.DeviceUnsupported(requirement = "Vulkan compute"),
            ParallaxError.PermissionDenied(permission = "android.permission.CAMERA"),
            ParallaxError.ThermalThrottled(severity = "Serious"),
            ParallaxError.StorageFailure(path = "/x", detail = "read-only"),
            ParallaxError.ProjectNotFound(projectId = "p"),
            ParallaxError.BundleInvalid(detail = "no metadata"),
            ParallaxError.ExportFailed(format = "USDZ", detail = "no mesh"),
            ParallaxError.EngineFailure(operation = "fuse", detail = "out of memory"),
            ParallaxError.Cancelled(),
            ParallaxError.Unknown(detail = "?"),
        )
        for (candidate in errors) {
            assertThat(candidate.message).isNotEmpty()
            // No message may blame the user or fall back to "something went wrong".
            assertThat(candidate.message.lowercase()).doesNotContain("something went wrong")
        }
    }

    @Test
    fun `the errors the user can act on offer the action that helps`() {
        // This is the point of the type: a twenty-minute capture that fails must not end at a
        // dead end, so each of these maps to exactly one button.
        assertThat(ParallaxError.InsufficientStorage(100, 40).recovery)
            .isEqualTo(RecoveryAction.FreeStorage(bytesNeeded = 60))
        assertThat(ParallaxError.PermissionDenied("android.permission.CAMERA").recovery)
            .isEqualTo(RecoveryAction.GrantPermission("android.permission.CAMERA"))
        assertThat(ParallaxError.CaptureTooShort(frameCount = 3, minimumFrames = 20).recovery)
            .isNotEqualTo(RecoveryAction.None)
        assertThat(ParallaxError.ThermalThrottled("Serious").recovery)
            .isNotEqualTo(RecoveryAction.None)
    }

    @Test
    fun `a shortfall never reads as negative`() {
        assertThat(ParallaxError.InsufficientStorage(10, 100).shortfallBytes).isEqualTo(0)
    }

    @Test
    fun `an io failure is classified as storage rather than unknown`() {
        val error = java.io.IOException("read-only file system").toParallaxError()
        assertThat(error).isInstanceOf(ParallaxError.StorageFailure::class.java)
    }

    @Test
    fun `a wrapped error survives the exception boundary unchanged`() {
        val original: ParallaxError = ParallaxError.BundleInvalid("no metadata")
        assertThat(ParallaxException(original).toParallaxError()).isEqualTo(original)
    }
}
