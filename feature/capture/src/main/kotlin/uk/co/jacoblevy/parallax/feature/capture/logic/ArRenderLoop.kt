package uk.co.jacoblevy.parallax.feature.capture.logic

/** What the render loop should do with the AR session on this frame. */
enum class ArLoopStep {
    /** The viewfinder is gone: close the session and stop. */
    Close,

    /** Stop the session, but keep it. */
    Pause,

    /** Create it if needed, and resume it. */
    Resume,

    /** The session is running: pull a frame. */
    Draw,

    /** Nothing to do, and nothing to draw. */
    Idle,
}

/**
 * What to do with the AR session on each frame, as a decision rather than a sequence of flags.
 *
 * This lived inside the renderer, where it could not be tested, and it was wrong: after a pause
 * had been applied, the next frame found no pending request and fell through to drawing --
 * checking only that the session object existed, not that it was running. ARCore answers
 * `update()` on a paused session with `SessionPausedException`, which ended the capture with
 * "scanning stopped unexpectedly" every time the screen was paused and came back.
 *
 * The decision is pure, so the mistake is now a failing test rather than a report from a phone.
 */
object ArRenderLoop {

    fun step(
        released: Boolean,
        pauseRequested: Boolean,
        resumeRequested: Boolean,
        sessionResumed: Boolean,
    ): ArLoopStep = when {
        // Releasing wins over everything: the surface is going away, and a session left open
        // is one ARCore still counts, which is fatal to the next scan.
        released -> ArLoopStep.Close
        pauseRequested -> ArLoopStep.Pause
        resumeRequested -> ArLoopStep.Resume
        // Running, not merely existing. This is the line that was wrong.
        sessionResumed -> ArLoopStep.Draw
        else -> ArLoopStep.Idle
    }
}
