package uk.co.jacoblevy.parallax.feature.capture.ui

import android.content.Context
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.view.Display
import android.util.Log
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import uk.co.jacoblevy.parallax.feature.capture.session.ArCaptureSession
import uk.co.jacoblevy.parallax.feature.capture.session.ArFrame
import uk.co.jacoblevy.parallax.feature.capture.session.ArSessionFailure
import uk.co.jacoblevy.parallax.core.ar.CameraBackgroundRenderer
import uk.co.jacoblevy.parallax.feature.capture.session.CaptureEngine
import uk.co.jacoblevy.parallax.feature.capture.session.CaptureTick
import java.nio.ByteBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * The live camera view, with ARCore driving it.
 *
 * A `GLSurfaceView` rather than anything Compose-native, because ARCore writes the camera image
 * into an external OES texture that only GL can sample. The renderer's thread is also where
 * ARCore's frames have to be consumed, so the capture loop runs there and hands each result back
 * through [onTick] -- which is why that callback must be cheap and must not block.
 */
@Composable
internal fun ArViewfinder(
    preferDepth: Boolean,
    recording: Boolean,
    engineProvider: () -> CaptureEngine?,
    onTick: (CaptureTick) -> Unit,
    onFailure: (ArSessionFailure, String?) -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current

    // The callbacks are read from the GL thread for the life of the session, so they are held
    // by a state that recomposition updates rather than captured once: a renderer built on the
    // first composition would otherwise go on calling the first `onTick` forever.
    val currentEngineProvider by rememberUpdatedState(engineProvider)
    val currentOnTick by rememberUpdatedState(onTick)
    val currentOnFailure by rememberUpdatedState(onFailure)

    // Remembered with no key. ARCore permits one session per process, so a renderer rebuilt on
    // any input -- `preferDepth` flipping when the capability check comes back, or the
    // mode, which it never needed -- would construct a second session beside the live one and
    // take the process down in native code, with no exception for anything to catch. What can
    // change is pushed in below instead.
    val renderer = remember {
        ArViewfinderRenderer(
            context = context,
            engineProvider = { currentEngineProvider() },
            onTick = { currentOnTick(it) },
            onFailure = { failure, detail -> currentOnFailure(failure, detail) },
        )
    }
    renderer.recording = recording
    renderer.preferDepth = preferDepth

    val view = remember(renderer) {
        GLSurfaceView(context).apply {
            addOnAttachStateChangeListener(
                object : android.view.View.OnAttachStateChangeListener {
                    override fun onViewAttachedToWindow(v: android.view.View) {
                        renderer.display = v.display
                    }

                    override fun onViewDetachedFromWindow(v: android.view.View) = Unit
                },
            )
            preserveEGLContextOnPause = true
            setEGLContextClientVersion(GL_VERSION)
            // 8/8/8 with no depth or stencil: the only thing drawn here is the camera quad.
            setEGLConfigChooser(8, 8, 8, 8, 0, 0)
            setRenderer(renderer)
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            setWillNotDraw(false)
        }
    }

    DisposableEffect(lifecycleOwner, view) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> {
                    renderer.requestResume()
                    view.onResume()
                }
                Lifecycle.Event.ON_PAUSE -> {
                    // Order matters: the GL thread has to stop before the session is paused,
                    // or ARCore is left updating against a dead context.
                    view.onPause()
                    renderer.requestPause()
                }
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            view.onPause()
            renderer.release()
        }
    }

    AndroidView(factory = { view }, modifier = modifier)
}

private const val GL_VERSION = 2

/**
 * The GL renderer, and the capture loop's thread.
 *
 * Every ARCore call happens here. The flags are plain `@Volatile` booleans rather than a
 * synchronised structure because they are set from the main thread and read once per frame
 * here, and a lock around a per-frame read is a cost with no benefit when the worst case of a
 * late read is one frame's delay.
 */
private class ArViewfinderRenderer(
    private val context: Context,
    private val engineProvider: () -> CaptureEngine?,
    private val onTick: (CaptureTick) -> Unit,
    private val onFailure: (ArSessionFailure, String?) -> Unit,
) : GLSurfaceView.Renderer {

    private val background = CameraBackgroundRenderer()
    private val session = ArCaptureSession(context)

    @Volatile
    var recording: Boolean = false

    /**
     * Whether to ask ARCore for depth. Set from the main thread and read on the GL thread when
     * the session is configured, so a capability check that finishes after the viewfinder is
     * already up still takes effect without rebuilding anything.
     */
    @Volatile
    var preferDepth: Boolean = false

    @Volatile
    private var resumeRequested = false

    @Volatile
    private var pauseRequested = false

    @Volatile
    private var released = false

    /** Set once a callback has thrown. The renderer draws nothing more after that. */
    @Volatile
    private var fatal = false

    private var viewWidth = 0
    private var viewHeight = 0
    private var reportedFailure: ArSessionFailure? = null

    /**
     * The display the surface is on.
     *
     * Taken from the view rather than from `Context.getDisplay`, which is API 30: minSdk is 29,
     * and the view's display is also the correct one on a foldable or an external screen, where
     * the context's default display may not be where this surface actually is.
     */
    @Volatile
    var display: Display? = null

    fun requestResume() {
        resumeRequested = true
        pauseRequested = false
    }

    fun requestPause() {
        pauseRequested = true
        resumeRequested = false
    }

    /**
     * Give up the session, from the main thread.
     *
     * The close happens here rather than on the GL thread, because by the time this is called
     * the surface has already been paused and the GL thread is not going to run again -- which
     * is how the session used to leak: the close was queued for a thread that was already
     * stopped, and the next scan then met a session ARCore still considered open.
     */
    fun release() {
        released = true
        session.close()
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) = guarded {
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        background.createOnGlThread()
        if (session.create(preferDepth)) {
            session.setCameraTexture(background.textureId)
        } else {
            report(session.failure ?: ArSessionFailure.Unknown)
        }
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) = guarded {
        GLES20.glViewport(0, 0, width, height)
        viewWidth = width
        viewHeight = height
        session.setDisplayGeometry(display, width, height)
    }

    override fun onDrawFrame(gl: GL10?) = guarded {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        if (!serviceLifecycle()) return@guarded

        val frame = session.update()
        if (frame == null) {
            session.failure?.let(::report)
            return@guarded
        }
        background.updateGeometry(frame.raw)
        background.draw()

        if (!recording) return@guarded
        val engine = engineProvider() ?: return@guarded
        processFrame(frame, engine)
    }

    /**
     * Run [body] on the GL thread without letting it end the process.
     *
     * Anything thrown from a `GLSurfaceView.Renderer` callback propagates out of the GL thread,
     * which kills the app outright: no screen, no dialog on some devices, and nothing for the
     * crash reporter to show because the report is written and then the thread that would have
     * displayed it is gone. The whole of capture is behind drivers, a camera service and native
     * ARCore, which between them throw things no signature admits to. So the callbacks report
     * instead of dying, and the scan stops with an explanation on screen.
     *
     * Once one has failed the renderer stops: a GL context that threw is not a context worth
     * drawing another frame against, and repeating the failure every frame would bury it.
     */
    private inline fun guarded(body: () -> Unit) {
        if (fatal) return
        try {
            body()
        } catch (@Suppress("TooGenericExceptionCaught") unexpected: Throwable) {
            fatal = true
            Log.e(TAG, "the capture renderer failed", unexpected)
            runCatching { session.close() }
            // The exception's own description goes to the screen, not only to logcat. A
            // device nobody here has is the only place these happen, and "stopped
            // unexpectedly" with no detail is a dead end for whoever is holding it.
            report(ArSessionFailure.Internal, describe(unexpected))
        }
    }

    /**
     * Apply any pending lifecycle request.
     *
     * @return false when there is nothing to draw this frame, which is the case while the
     *   session is closed or has just been paused.
     */
    private fun serviceLifecycle(): Boolean {
        if (released) {
            session.close()
            return false
        }
        if (pauseRequested) {
            session.pause()
            pauseRequested = false
            return false
        }
        if (!resumeRequested) return session.isRunning

        resumeRequested = false
        if (!session.isRunning && !session.create(preferDepth)) {
            report(session.failure ?: ArSessionFailure.Unknown)
            return false
        }
        session.setCameraTexture(background.textureId)
        if (viewWidth > 0) {
            session.setDisplayGeometry(display, viewWidth, viewHeight)
        }
        if (!session.resume()) {
            report(session.failure ?: ArSessionFailure.CameraUnavailable)
            return false
        }
        return true
    }

    /**
     * Hand one frame to the capture loop.
     *
     * The images are closed on every path. ARCore holds a small fixed pool, and a leaked one
     * stalls the session within a handful of frames -- the symptom is capture freezing after
     * about two seconds with nothing in the log.
     */
    private fun processFrame(
        frame: ArFrame,
        engine: CaptureEngine,
    ) {
        val image = frame.acquireCameraImage() ?: return
        val depth = frame.acquireDepthImage()
        try {
            val luma = image.planes.firstOrNull()?.buffer ?: return
            engine.setSubjectDistance(frame.centreDepthMetres())
            engine.updatePlanes(session.trackedPlanes())
            val tick = engine.onFrame(
                nowMillis = System.currentTimeMillis(),
                timestampNanos = frame.timestampNanos,
                trackingState = frame.trackingState,
                pose = frame.pose,
                intrinsics = frame.intrinsics,
                luma = luma.asDirectBuffer(),
                lumaWidth = image.width,
                lumaHeight = image.height,
                lumaRowStride = image.planes[0].rowStride,
                image = image,
                depth = depth,
            )
            onTick(tick)
        } catch (failure: RuntimeException) {
            // A frame that cannot be processed must not take the GL thread down: the session is
            // still good, and the next frame usually works.
            Log.w(TAG, "dropping a frame that could not be processed", failure)
        } finally {
            depth?.close()
            image.close()
        }
    }

    private fun report(failure: ArSessionFailure, detail: String? = null) {
        if (reportedFailure == failure) return
        reportedFailure = failure
        onFailure(failure, detail)
    }

    /**
     * One line naming what went wrong, for the screen.
     *
     * The class name is included because the message is often null -- a `NullPointerException`
     * or an `UnsatisfiedLinkError` says everything in its type -- and the root cause is used
     * because the top of the chain is usually the wrapper, not the fault.
     */
    private fun describe(throwable: Throwable): String {
        val root = generateSequence(throwable, Throwable::cause).last()
        val type = root.javaClass.simpleName
        return root.message?.takeIf(String::isNotBlank)?.let { "$type: $it" } ?: type
    }

    /**
     * ARCore's plane buffers are already direct, but a defensive copy is made if one ever is
     * not: the native scorer requires a direct buffer and would otherwise throw on every frame.
     */
    private fun ByteBuffer.asDirectBuffer(): ByteBuffer {
        if (isDirect) return this
        val copy = ByteBuffer.allocateDirect(remaining())
        copy.put(this)
        copy.rewind()
        return copy
    }

    private companion object {
        const val TAG = "ArViewfinder"
    }
}
