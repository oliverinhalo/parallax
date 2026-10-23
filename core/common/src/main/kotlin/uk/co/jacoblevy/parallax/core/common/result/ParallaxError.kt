package uk.co.jacoblevy.parallax.core.common.result

import java.io.IOException

/**
 * Every way the app can fail, paired with what the user can do about it.
 *
 * The [recovery] is the point of this type. A scan that fails after twenty minutes of capture
 * must not leave the user at "Something went wrong"; it must say which stage failed, whether the
 * work so far survived, and offer the one action that will actually help.
 */
sealed class ParallaxError {
    /** What to show the user. Free of jargon and never blames them. */
    abstract val message: String

    /** The single most useful thing the user can do next. */
    open val recovery: RecoveryAction = RecoveryAction.None

    /** True when retrying the identical operation could plausibly succeed. */
    open val retryable: Boolean = false

    data class InsufficientStorage(
        val requiredBytes: Long,
        val availableBytes: Long,
    ) : ParallaxError() {
        val shortfallBytes: Long get() = (requiredBytes - availableBytes).coerceAtLeast(0)
        override val message: String
            get() = "Not enough space to finish this scan."
        override val recovery: RecoveryAction
            get() = RecoveryAction.FreeStorage(shortfallBytes)
    }

    data class CaptureTooShort(val frameCount: Int, val minimumFrames: Int) : ParallaxError() {
        override val message: String
            get() = "This scan has only $frameCount usable frames; at least $minimumFrames " +
                "are needed to reconstruct geometry."
        override val recovery: RecoveryAction get() = RecoveryAction.Recapture
    }

    data class TrackingLost(val recoveredFrames: Int) : ParallaxError() {
        override val message: String
            get() = "Tracking was lost during capture, so the camera path is incomplete."
        override val recovery: RecoveryAction
            get() = if (recoveredFrames > 0) {
                RecoveryAction.ProcessPartial(recoveredFrames)
            } else {
                RecoveryAction.Recapture
            }
    }

    /** A reconstruction stage failed. Earlier stages are checkpointed, so we can resume. */
    data class ProcessingFailed(
        val stage: String,
        val detail: String,
        val resumableFromStage: String?,
    ) : ParallaxError() {
        override val message: String get() = "Processing stopped during $stage."
        override val recovery: RecoveryAction
            get() = resumableFromStage?.let { RecoveryAction.ResumeFrom(it) }
                ?: RecoveryAction.Retry
        override val retryable: Boolean get() = true
    }

    data class ReconstructionEmpty(val stage: String) : ParallaxError() {
        override val message: String
            get() = "There was not enough overlap between frames to build a model."
        override val recovery: RecoveryAction get() = RecoveryAction.Recapture
    }

    data class DeviceUnsupported(val requirement: String) : ParallaxError() {
        override val message: String get() = "This device does not support $requirement."
        override val recovery: RecoveryAction get() = RecoveryAction.None
    }

    data class PermissionDenied(val permission: String) : ParallaxError() {
        override val message: String get() = "Parallax needs permission to use the camera."
        override val recovery: RecoveryAction get() = RecoveryAction.GrantPermission(permission)
    }

    data class ThermalThrottled(val severity: String) : ParallaxError() {
        override val message: String
            get() = "The phone is too warm to keep processing at full speed."
        override val recovery: RecoveryAction get() = RecoveryAction.WaitAndRetry
        override val retryable: Boolean get() = true
    }

    data class StorageFailure(val path: String, val detail: String) : ParallaxError() {
        override val message: String get() = "A project file could not be read or written."
        override val recovery: RecoveryAction get() = RecoveryAction.Retry
        override val retryable: Boolean get() = true
    }

    data class ProjectNotFound(val projectId: String) : ParallaxError() {
        override val message: String get() = "That project is no longer on this device."
        override val recovery: RecoveryAction get() = RecoveryAction.GoToLibrary
    }

    data class BundleInvalid(val detail: String) : ParallaxError() {
        override val message: String get() = "That file is not a Parallax project bundle."
        override val recovery: RecoveryAction get() = RecoveryAction.None
    }

    data class ExportFailed(val format: String, val detail: String) : ParallaxError() {
        override val message: String get() = "The $format export could not be written."
        override val recovery: RecoveryAction get() = RecoveryAction.Retry
        override val retryable: Boolean get() = true
    }

    data class EngineFailure(val operation: String, val detail: String) : ParallaxError() {
        override val message: String get() = "The reconstruction engine reported a problem."
        override val recovery: RecoveryAction get() = RecoveryAction.Retry
        override val retryable: Boolean get() = true
    }

    data class Cancelled(val stage: String? = null) : ParallaxError() {
        override val message: String get() = "Cancelled."
    }

    data class Unknown(val detail: String) : ParallaxError() {
        override val message: String get() = "Something unexpected went wrong."
        override val recovery: RecoveryAction get() = RecoveryAction.Retry
        override val retryable: Boolean get() = true
    }
}

/** The action an error screen offers. Each maps to exactly one button. */
sealed interface RecoveryAction {
    data object None : RecoveryAction
    data object Retry : RecoveryAction
    data object WaitAndRetry : RecoveryAction
    data object Recapture : RecoveryAction
    data object GoToLibrary : RecoveryAction
    data class FreeStorage(val bytesNeeded: Long) : RecoveryAction
    data class ResumeFrom(val stage: String) : RecoveryAction
    data class ProcessPartial(val usableFrames: Int) : RecoveryAction
    data class GrantPermission(val permission: String) : RecoveryAction
}

/** Best-effort classification of an exception that escaped a lower layer. */
fun Throwable.toParallaxError(): ParallaxError = when (this) {
    is ParallaxException -> error
    is OutOfMemoryError -> ParallaxError.ProcessingFailed(
        stage = "unknown",
        detail = "out of memory",
        resumableFromStage = null,
    )
    is IOException -> ParallaxError.StorageFailure(
        path = "",
        detail = message ?: this::class.simpleName.orEmpty(),
    )
    else -> ParallaxError.Unknown(message ?: this::class.simpleName.orEmpty())
}

/** Thrown when a [ParallaxError] has to cross a boundary that only speaks exceptions. */
class ParallaxException(val error: ParallaxError) : Exception(error.message)
