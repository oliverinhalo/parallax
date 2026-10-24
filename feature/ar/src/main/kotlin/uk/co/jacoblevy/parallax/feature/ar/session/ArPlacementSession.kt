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
            failure = ArPlacementFailure(e.message ?: "AR is not available on this device.")
            false
        } catch (e: SecurityException) {
            Log.w(TAG, "no camera permission for AR placement", e)
            failure = ArPlacementFailure("Camera permission is needed to place a scan in a room.")
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
