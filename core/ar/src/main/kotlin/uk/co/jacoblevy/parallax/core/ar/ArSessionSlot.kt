package uk.co.jacoblevy.parallax.core.ar

import java.util.concurrent.atomic.AtomicReference

/**
 * The one ARCore session a process may have.
 *
 * ARCore allows a single session per process and enforces it in native code, which means
 * breaking the rule does not throw: the process dies, with no Kotlin exception for any handler
 * to see and nothing in the app able to report it. Two bugs reached a device that way -- a
 * renderer rebuilt on a mode change, and a session never closed when the capture screen
 * closed -- and both looked to the user like the app silently vanishing.
 *
 * So the rule is held here instead of by the care of each call site. A second claim fails,
 * cleanly, and the screen says the camera is still busy rather than taking the process down.
 *
 * It lives in `core:ar` rather than in capture because capture is not the only claimant: AR
 * placement opens a session of its own, so a scan whose session outlived its screen would
 * otherwise kill the app the moment a finished model was placed in a room. The only state in
 * the app that is deliberately process-wide, because the thing it guards is process-wide too.
 */
object ArSessionSlot {

    private val holder = AtomicReference<Any?>(null)

    /** Claims the slot for [candidate]. False when someone else already holds it. */
    fun acquire(candidate: Any): Boolean = holder.compareAndSet(null, candidate)

    /** Releases the slot, but only if [candidate] is the holder. */
    fun release(candidate: Any) {
        holder.compareAndSet(candidate, null)
    }

    fun isHeldBy(candidate: Any): Boolean = holder.get() === candidate

    val isHeld: Boolean get() = holder.get() != null
}
