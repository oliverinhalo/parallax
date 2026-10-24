package uk.co.jacoblevy.parallax.feature.capture.session

import android.content.Context
import android.media.Image
import android.util.Log
import android.view.Display
import com.google.ar.core.Camera
import com.google.ar.core.Config
import com.google.ar.core.Frame
import com.google.ar.core.Plane
import com.google.ar.core.Session
import com.google.ar.core.TrackingState as ArTrackingState
import com.google.ar.core.exceptions.CameraNotAvailableException
import com.google.ar.core.exceptions.NotYetAvailableException
import com.google.ar.core.exceptions.UnavailableException
import uk.co.jacoblevy.parallax.core.model.CameraIntrinsics
import uk.co.jacoblevy.parallax.core.model.DetectedPlane
import uk.co.jacoblevy.parallax.core.model.PlaneKind
import uk.co.jacoblevy.parallax.core.model.Pose
import uk.co.jacoblevy.parallax.core.model.TrackingState

/** One ARCore update, converted into domain types the rest of the app already speaks. */
class ArFrame internal constructor(
    /**
     * The ARCore frame itself.
     *
     * Exposed only so the background renderer can ask it for the display geometry: that mapping
     * from view coordinates to texture coordinates is ARCore's to compute, and deriving it by
     * hand from the display rotation is where the classic "preview is sideways on one phone"
     * bug comes from.
     */
    internal val raw: Frame,
    val trackingState: TrackingState,
    val pose: Pose?,
    val intrinsics: CameraIntrinsics?,
    val timestampNanos: Long,
) {
    /**
     * The camera image, which the caller must close.
     *
     * ARCore holds a small fixed pool of these, and a leaked one stalls the session within a
     * handful of frames -- the symptom is capture freezing after about two seconds, with nothing
     * in the log.
     */
    fun acquireCameraImage(): Image? = try {
        raw.acquireCameraImage()
    } catch (expected: NotYetAvailableException) {
        // Normal for the first frames after a resume: ARCore has not filled the pool yet.
        null
    }

    /** The depth map, where the device has one. Also must be closed by the caller. */
    fun acquireDepthImage(): Image? = try {
        raw.acquireDepthImage16Bits()
    } catch (expected: NotYetAvailableException) {
        null
    } catch (expected: IllegalStateException) {
        // Thrown when depth is not configured, which is most devices.
        null
    }

    /** Distance to whatever is in the middle of the frame, from depth or from a plane hit. */
    fun centreDepthMetres(): Float? = runCatching {
        val hits = raw.hitTest(CENTRE_FRACTION, CENTRE_FRACTION)
        hits.firstOrNull { it.trackable is Plane }?.distance
            ?: hits.firstOrNull()?.distance
    }.getOrNull()

    private companion object {
        /** Hit-testing uses view coordinates; the centre is half the extent in each axis. */
        const val CENTRE_FRACTION = 0.5f
    }
}

/**
 * Wraps the ARCore session.
 *
 * ARCore's lifecycle is strict and its failures are mostly recoverable, so each one is reported
 * rather than thrown: the camera being busy because another app has it, ARCore needing an
 * update, the session needing a GL texture before it will produce a frame. The capture screen
 * shows the reason and offers the fix.
 *
 * Not thread-safe, and deliberately so: every method here must be called from the GL thread that
 * owns the session's texture, which is the one constraint ARCore will not forgive.
 */
class ArCaptureSession(private val context: Context) {

    private var session: Session? = null
    private var textureSet = false

    var failure: ArSessionFailure? = null
        private set

    val isRunning: Boolean get() = session != null

    /** True when the session was configured with depth, which the device may refuse. */
    var depthEnabled: Boolean = false
        private set

    /**
     * Create and configure the session.
     *
     * @param preferDepth ask for depth; silently falls back when the device cannot do it, since
     *   a scan without depth still works through monocular estimation.
     */
    fun create(preferDepth: Boolean): Boolean {
        if (session != null) return true
        return try {
            val created = Session(context)
            val config = created.config.apply {
                // LATEST_CAMERA_IMAGE, not BLOCKING: capture drives its own frame rate from the
                // thermal budget, and a blocking update would peg it to the camera instead.
                updateMode = Config.UpdateMode.LATEST_CAMERA_IMAGE
                focusMode = Config.FocusMode.AUTO
                planeFindingMode = Config.PlaneFindingMode.HORIZONTAL_AND_VERTICAL
                lightEstimationMode = Config.LightEstimationMode.AMBIENT_INTENSITY
                depthMode = if (preferDepth &&
                    created.isDepthModeSupported(Config.DepthMode.AUTOMATIC)
                ) {
                    Config.DepthMode.AUTOMATIC
                } else {
                    Config.DepthMode.DISABLED
                }
            }
            created.configure(config)
            depthEnabled = config.depthMode != Config.DepthMode.DISABLED
            session = created
            failure = null
            true
        } catch (unavailable: UnavailableException) {
            // Classified rather than logged: every case here has a specific thing the user can
            // do about it, which `ArSessionFailure` carries.
            failure = ArSessionFailure.from(unavailable)
            false
        } catch (expected: SecurityException) {
            // The camera permission was revoked while the screen was open.
            failure = ArSessionFailure.PermissionRevoked
            false
        }
    }

    /** Hand ARCore the texture it writes the camera image into. Call on the GL thread. */
    fun setCameraTexture(textureId: Int) {
        session?.setCameraTextureName(textureId)
        textureSet = true
    }

    fun setDisplayGeometry(display: Display?, widthPx: Int, heightPx: Int) {
        val rotation = display?.rotation ?: 0
        session?.setDisplayGeometry(rotation, widthPx, heightPx)
    }

    fun resume(): Boolean {
        val current = session ?: return false
        return try {
            current.resume()
            true
        } catch (expected: CameraNotAvailableException) {
            // Another app has the camera. Reported, not logged: the UI says so and offers a
            // retry, which is more use than a line in logcat.
            failure = ArSessionFailure.CameraUnavailable
            false
        }
    }

    fun pause() {
        runCatching { session?.pause() }
            .onFailure { Log.w(TAG, "pausing the AR session failed", it) }
    }

    fun close() {
        runCatching { session?.close() }
            .onFailure { Log.w(TAG, "closing the AR session failed", it) }
        session = null
        textureSet = false
    }

    /**
     * Pull the next frame. Returns null when there is nothing new, which is normal.
     *
     * ARCore will not produce a frame before it has a texture, and calling `update` without one
     * throws, so the guard is not defensive padding.
     */
    fun update(): ArFrame? {
        val current = session ?: return null
        if (!textureSet) return null
        val frame = try {
            current.update()
        } catch (expected: CameraNotAvailableException) {
            failure = ArSessionFailure.CameraUnavailable
            return null
        }
        val camera = frame.camera
        return ArFrame(
            raw = frame,
            trackingState = camera.trackingState.toDomain(),
            pose = camera.poseOrNull(),
            intrinsics = camera.intrinsicsOrNull(),
            timestampNanos = frame.timestamp,
        )
    }

    /** Planes ARCore is currently tracking, for room mode and the ground cutout. */
    fun trackedPlanes(): List<DetectedPlane> {
        val current = session ?: return emptyList()
        return current.getAllTrackables(Plane::class.java)
            .filter { it.trackingState == ArTrackingState.TRACKING }
            .map { plane ->
                val centre = plane.centerPose
                val normal = plane.centerPose.yAxis
                DetectedPlane(
                    // Plane identity has to survive across frames for the floor plan to
                    // accumulate, and ARCore has no stable id, so the subsuming chain's root
                    // identity hash is used: a merged plane keeps its parent's key.
                    id = planeKey(plane),
                    centerX = centre.tx(),
                    centerY = centre.ty(),
                    centerZ = centre.tz(),
                    normalX = normal[0],
                    normalY = normal[1],
                    normalZ = normal[2],
                    extentX = plane.extentX,
                    extentZ = plane.extentZ,
                    kind = when (plane.type) {
                        Plane.Type.HORIZONTAL_UPWARD_FACING -> PlaneKind.HorizontalUpward
                        Plane.Type.HORIZONTAL_DOWNWARD_FACING -> PlaneKind.HorizontalDownward
                        Plane.Type.VERTICAL -> PlaneKind.Vertical
                    },
                )
            }
    }

    private fun planeKey(plane: Plane): String {
        var root: Plane = plane
        var guard = 0
        // Walk to the root of the subsumption chain, with a bound in case ARCore ever hands
        // back a cycle; an unbounded walk here would hang the GL thread.
        while (guard++ < MAX_SUBSUME_DEPTH) {
            val parent = root.subsumedBy ?: break
            root = parent
        }
        return System.identityHashCode(root).toString()
    }

    private fun ArTrackingState.toDomain(): TrackingState = when (this) {
        ArTrackingState.TRACKING -> TrackingState.Tracking
        ArTrackingState.PAUSED -> TrackingState.Limited
        ArTrackingState.STOPPED -> TrackingState.Lost
    }

    private fun Camera.poseOrNull(): Pose? {
        if (trackingState != ArTrackingState.TRACKING) return null
        val matrix = FloatArray(Pose.MATRIX_ELEMENTS)
        // World-from-camera, column-major: the same convention the manifest, the native engine
        // and Filament all use, so nothing has to be transposed at a boundary.
        displayOrientedPose.toMatrix(matrix, 0)
        return Pose(matrix)
    }

    private fun Camera.intrinsicsOrNull(): CameraIntrinsics? {
        if (trackingState != ArTrackingState.TRACKING) return null
        val intrinsics = imageIntrinsics
        val focal = intrinsics.focalLength
        val principal = intrinsics.principalPoint
        val dimensions = intrinsics.imageDimensions
        return CameraIntrinsics(
            focalLengthX = focal[0],
            focalLengthY = focal[1],
            principalPointX = principal[0],
            principalPointY = principal[1],
            imageWidth = dimensions[0],
            imageHeight = dimensions[1],
        )
    }

    private companion object {
        const val TAG = "ArCaptureSession"
        const val MAX_SUBSUME_DEPTH = 32
    }
}

/** Why the AR session could not start, and what the user can do about it. */
enum class ArSessionFailure(val message: String, val userAction: String) {
    NeedsInstall(
        message = "ARCore is not installed.",
        userAction = "Install ARCore to scan in this mode, or use Import instead.",
    ),
    NeedsUpdate(
        message = "ARCore needs updating.",
        userAction = "Update ARCore, or use Import instead.",
    ),
    DeviceUnsupported(
        message = "This device does not support AR scanning.",
        userAction = "Import mode works on any device and needs no AR support.",
    ),
    SdkTooOld(
        message = "This build of Parallax is too old for the installed ARCore.",
        userAction = "Update Parallax.",
    ),
    CameraUnavailable(
        message = "The camera is in use by another app.",
        userAction = "Close the other app and try again.",
    ),
    PermissionRevoked(
        message = "Camera access was turned off.",
        userAction = "Grant camera access in settings.",
    ),
    Unknown(
        message = "AR scanning could not start.",
        userAction = "Try again, or use Import instead.",
    ),
    ;

    companion object {
        fun from(exception: UnavailableException): ArSessionFailure = when (exception) {
            is com.google.ar.core.exceptions.UnavailableArcoreNotInstalledException -> NeedsInstall
            is com.google.ar.core.exceptions.UnavailableApkTooOldException -> NeedsUpdate
            is com.google.ar.core.exceptions.UnavailableDeviceNotCompatibleException ->
                DeviceUnsupported
            is com.google.ar.core.exceptions.UnavailableSdkTooOldException -> SdkTooOld
            is com.google.ar.core.exceptions.UnavailableUserDeclinedInstallationException ->
                NeedsInstall
            else -> Unknown
        }
    }
}
