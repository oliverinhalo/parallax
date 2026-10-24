package uk.co.jacoblevy.parallax.feature.ar.session

import android.content.Context
import android.util.Log
import com.google.ar.core.Config
import com.google.ar.core.Frame
import com.google.ar.core.HitResult
import com.google.ar.core.Plane
import com.google.ar.core.Session
import com.google.ar.core.Trackable
import com.google.ar.core.TrackingState
import com.google.ar.core.exceptions.CameraNotAvailableException
import com.google.ar.core.exceptions.UnavailableException
import uk.co.jacoblevy.parallax.core.ar.ArSessionSlot

/** Why AR placement could not start, phrased for a person. */
data class ArPlacementFailure(val message: String)

/** A point on a tracked surface, in AR world coordinates. */
data class SurfaceHit(val x: Float, val y: Float, val z: Float)

/**
 * ARCore, configured for putting a model on the floor.
 *
 * Separate from the capture session rather than sharing it: this one wants
 * horizontal plane finding and no depth, and that one wants depth and no
 * plane finding. The two sets of flags are nearly disjoint, so one session
 * class covering both would be a pile of booleans with a comment explaining
 * which combinations are real.
 */
class ArPlacementSession(private val context: Context) {

    private companion object {
        const val TAG = "ArPlacementSession"
    }

    private var session: Session? = null
    private var textureSet = false

    var failure: ArPlacementFailure? = null
        private set

    val isRunning: Boolean get() = session != null

    fun start(): Boolean {
        if (session != null) return true
        failure = null
        // ARCore permits one session per process and enforces it in native code, so a capture
        // session that has not finished closing has to be reported rather than walked into.
        if (!ArSessionSlot.acquire(this)) {
            failure = ArPlacementFailure("The camera is still busy. Go back and try again.")
            return false
        }
        return try {
            val created = Session(context)
            created.configure(
                created.config.apply {
                    // Horizontal planes only: a scan sits on the floor or a
                    // table, and offering walls as targets makes the hit test
                    // pick one by accident when the user is aiming at the floor.
                    planeFindingMode = Config.PlaneFindingMode.HORIZONTAL
                    lightEstimationMode = Config.LightEstimationMode.AMBIENT_INTENSITY
                    updateMode = Config.UpdateMode.LATEST_CAMERA_IMAGE
                    focusMode = Config.FocusMode.AUTO
                },
            )
            session = created
            true
        } catch (e: UnavailableException) {
            Log.w(TAG, "ARCore refused to start a placement session", e)
            ArSessionSlot.release(this)
            failure = ArPlacementFailure(e.message ?: "AR is not available on this device.")
            false
        } catch (e: SecurityException) {
            Log.w(TAG, "no camera permission for AR placement", e)
            ArSessionSlot.release(this)
            failure = ArPlacementFailure("Camera permission is needed to place a scan in a room.")
            false
        } catch (e: RuntimeException) {
            // Undocumented, device-specific, and on the GL thread -- where anything thrown
            // ends the process rather than reaching a catch higher up.
            Log.w(TAG, "starting the placement session failed", e)
            ArSessionSlot.release(this)
            failure = ArPlacementFailure("AR could not start on this device.")
            false
        }
    }

    /** Hand ARCore the texture it should write the camera image into. */
    fun setCameraTexture(textureId: Int) {
        val current = session ?: return
        if (textureSet) return
        current.setCameraTextureName(textureId)
        textureSet = true
    }

    fun resume(): Boolean {
        val current = session ?: return false
        return try {
            current.resume()
            true
        } catch (e: CameraNotAvailableException) {
            Log.w(TAG, "the camera was not available for AR placement", e)
            failure = ArPlacementFailure("The camera is in use by another app.")
            false
        }
    }

    fun pause() {
        runCatching { session?.pause() }
    }

    fun close() {
        pause()
        runCatching { session?.close() }
        session = null
        textureSet = false
        // Released even when closing threw: a slot held by a session nobody can reach is a
        // camera that never opens again for the rest of the process's life.
        ArSessionSlot.release(this)
    }

    fun setDisplayGeometry(rotation: Int, width: Int, height: Int) {
        runCatching { session?.setDisplayGeometry(rotation, width, height) }
    }

    /** The newest frame, or null when the session is not delivering any. */
    fun update(): Frame? = runCatching { session?.update() }.getOrNull()

    /** How many horizontal planes ARCore is tracking right now. */
    fun trackedPlaneCount(): Int {
        val current = session ?: return 0
        return runCatching {
            current.getAllTrackables(Plane::class.java).count {
                it.trackingState == TrackingState.TRACKING &&
                    it.type != Plane.Type.VERTICAL
            }
        }.getOrDefault(0)
    }

    /**
     * Where a tap lands on a tracked surface.
     *
     * Only hits inside a plane's own polygon count. ARCore will happily report
     * a hit on the infinite extension of a plane, which is how a model ends up
     * floating in the middle of the room when the user taps past the edge of
     * the bit of floor that has actually been scanned.
     */
    fun hitTest(frame: Frame, screenX: Float, screenY: Float): SurfaceHit? = runCatching {
        frame.hitTest(screenX, screenY)
            .asSequence()
            .filter { isUsableSurface(it.trackable, it) }
            .map { SurfaceHit(it.hitPose.tx(), it.hitPose.ty(), it.hitPose.tz()) }
            .firstOrNull()
    }.getOrNull()

    private fun isUsableSurface(trackable: Trackable?, hit: HitResult): Boolean {
        val plane = trackable as? Plane ?: return false
        if (plane.trackingState != TrackingState.TRACKING) return false
        if (plane.type == Plane.Type.VERTICAL) return false
        return plane.isPoseInPolygon(hit.hitPose)
    }
}
