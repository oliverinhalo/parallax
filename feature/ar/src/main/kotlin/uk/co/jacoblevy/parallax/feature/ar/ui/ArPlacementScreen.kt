package uk.co.jacoblevy.parallax.feature.ar.ui

import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.opengl.Matrix
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.viewinterop.AndroidView
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import uk.co.jacoblevy.parallax.core.ar.CameraBackgroundRenderer
import uk.co.jacoblevy.parallax.core.designsystem.component.GlassPanel
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxChip
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxIconButton
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.feature.ar.ArPlacementUiState
import uk.co.jacoblevy.parallax.feature.ar.ArPlacementViewModel
import uk.co.jacoblevy.parallax.feature.ar.render.ArModelRenderer
import uk.co.jacoblevy.parallax.feature.ar.render.PendingTap
import uk.co.jacoblevy.parallax.feature.ar.session.ArPlacementSession
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/** Entry point for the navigation graph. */
@Composable
fun ArPlacementRoute(
    onBack: () -> Unit,
    viewModel: ArPlacementViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val pendingTap = remember { PendingTap() }

    Box(Modifier.fillMaxSize()) {
        ArSurface(
            state = state,
            viewModel = viewModel,
            pendingTap = pendingTap,
            modifier = Modifier
                .fillMaxSize()
                .placementGestures(state, viewModel, pendingTap),
        )
        Overlay(state = state, viewModel = viewModel, onBack = onBack)
    }
}

/**
 * Gestures over the camera feed.
 *
 * A tap places; once placed, drag moves, pinch resizes and twist turns. The
 * drag is converted to metres from the model's own footprint rather than a
 * fixed factor, so a tabletop model and a life-size one both follow the
 * finger at about the same speed.
 */
private fun Modifier.placementGestures(
    state: ArPlacementUiState,
    viewModel: ArPlacementViewModel,
    pendingTap: PendingTap,
): Modifier = this
    .pointerInput(state.placement.isPlaced, state.placement.locked) {
        detectTransformGestures { _, pan, zoom, rotation ->
            if (!state.placement.isPlaced) return@detectTransformGestures
            if (zoom != 1f) viewModel.scale(zoom)
            if (rotation != 0f) viewModel.rotate(rotation * DEGREES_TO_RADIANS)
            if (pan != Offset.Zero) {
                val metresPerPixel =
                    state.placement.footprintMetres.first / size.width.coerceAtLeast(1)
                viewModel.move(pan.x * metresPerPixel, pan.y * metresPerPixel)
            }
        }
    }
    .pointerInput(Unit) {
        detectTapGestures { offset -> pendingTap.offset = offset }
    }

@Composable
private fun ArSurface(
    state: ArPlacementUiState,
    viewModel: ArPlacementViewModel,
    pendingTap: PendingTap,
    modifier: Modifier = Modifier,
) {
    val latest = rememberUpdatedState(state)
    val surfaceHolder = remember { mutableStateOf<GLSurfaceView?>(null) }
    val sessionHolder = remember { mutableStateOf<ArPlacementSession?>(null) }

    AndroidView(
        modifier = modifier.testTag("arSurface"),
        factory = { context ->
            val session = ArPlacementSession(context)
            if (!session.start()) {
                viewModel.sessionFailed(
                    session.failure?.message ?: "AR is not available on this device.",
                )
            }
            sessionHolder.value = session

            GLSurfaceView(context).apply {
                preserveEGLContextOnPause = true
                setEGLContextClientVersion(2)
                setEGLConfigChooser(8, 8, 8, 8, 16, 0)
                setRenderer(
                    PlacementRenderer(
                        view = this,
                        session = session,
                        viewModel = viewModel,
                        pendingTap = pendingTap,
                        state = latest::value,
                    ),
                )
                surfaceHolder.value = this
            }
        },
        onRelease = {
            sessionHolder.value?.close()
            sessionHolder.value = null
            surfaceHolder.value = null
        },
    )

    DisposableEffect(Unit) {
        sessionHolder.value?.let { session ->
            if (!session.resume()) {
                viewModel.sessionFailed(session.failure?.message ?: "The camera is unavailable.")
            }
        }
        surfaceHolder.value?.onResume()
        onDispose {
            surfaceHolder.value?.onPause()
            sessionHolder.value?.pause()
        }
    }
}

/**
 * The GL thread's whole job: camera feed, then the model if one is placed.
 *
 * It reads the state through a getter rather than holding a copy, because it
 * runs on its own thread and a captured value would be whatever the state was
 * when the surface was created.
 */
private class PlacementRenderer(
    private val view: GLSurfaceView,
    private val session: ArPlacementSession,
    private val viewModel: ArPlacementViewModel,
    private val pendingTap: PendingTap,
    private val state: () -> ArPlacementUiState,
) : GLSurfaceView.Renderer {

    private val background = CameraBackgroundRenderer()
    private val model = ArModelRenderer()
    private var uploaded = false
    private val viewMatrix = FloatArray(MATRIX_SIZE)
    private val projectionMatrix = FloatArray(MATRIX_SIZE)
    private val viewProjection = FloatArray(MATRIX_SIZE)
    private val columnMajorModel = FloatArray(MATRIX_SIZE)

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        background.createOnGlThread()
        model.createOnGlThread()
        session.setCameraTexture(background.textureId)
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
        session.setDisplayGeometry(view.display?.rotation ?: 0, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT or GLES20.GL_DEPTH_BUFFER_BIT)
        val frame = session.update() ?: return
        background.updateGeometry(frame)
        background.draw()

        val current = state()
        current.geometry?.let { geometry ->
            if (!uploaded) {
                model.setModel(geometry.vertices, geometry.indices, null, null)
                uploaded = true
            }
        }
        viewModel.surfacesTracked(session.trackedPlaneCount())

        pendingTap.offset?.let { offset ->
            pendingTap.offset = null
            session.hitTest(frame, offset.x, offset.y)?.let { hit ->
                viewModel.placeAt(hit.x, hit.y, hit.z)
            }
        }

        if (!current.placement.isPlaced || !model.hasModel) return
        frame.camera.getViewMatrix(viewMatrix, 0)
        frame.camera.getProjectionMatrix(projectionMatrix, 0, NEAR_METRES, FAR_METRES)
        Matrix.multiplyMM(viewProjection, 0, projectionMatrix, 0, viewMatrix, 0)
        transpose(current.placement.modelMatrix(), columnMajorModel)
        model.draw(columnMajorModel, viewProjection, 1f - current.transparency)
    }
}

@Composable
private fun Overlay(
    state: ArPlacementUiState,
    viewModel: ArPlacementViewModel,
    onBack: () -> Unit,
) {
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(horizontal = Spacing.screenHorizontal, vertical = Spacing.sm),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ParallaxIconButton(
                icon = ParallaxIcons.Back,
                contentDescription = "Back",
                onClick = onBack,
            )
            Spacer(Modifier.width(Spacing.sm))
            GlassPanel(
                contentPadding = PaddingValues(
                    horizontal = Spacing.md,
                    vertical = Spacing.sm,
                ),
            ) {
                Text(
                    text = state.guidance,
                    style = MaterialTheme.typography.bodyMedium,
                    color = ParallaxTheme.colors.textPrimary,
                )
            }
        }

        Spacer(Modifier.weight(1f))

        if (state.placement.isPlaced) {
            GlassPanel(
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = Spacing.screenHorizontal, vertical = Spacing.sm),
            ) {
                Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                    Text(
                        text = sizeSummary(state),
                        style = MaterialTheme.typography.bodyMedium,
                        color = ParallaxTheme.colors.textSecondary,
                    )
                    Text(
                        "See through it",
                        style = MaterialTheme.typography.labelLarge,
                        color = ParallaxTheme.colors.textSecondary,
                    )
                    Slider(
                        value = state.transparency,
                        onValueChange = viewModel::setTransparency,
                        valueRange = 0f..MAX_TRANSPARENCY,
                        modifier = Modifier.semantics { contentDescription = "Transparency" },
                    )
                }
            }
        }

        Row(
            Modifier
                .fillMaxWidth()
                .navigationBarsPadding()
                .padding(horizontal = Spacing.screenHorizontal, vertical = Spacing.md),
            horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
        ) {
            if (state.placement.isPlaced) {
                ParallaxChip(
                    text = "Square up",
                    selected = false,
                    onClick = viewModel::snapToRightAngle,
                )
                ParallaxChip(
                    text = if (state.placement.locked) "Unlock" else "Lock",
                    selected = state.placement.locked,
                    onClick = { viewModel.setLocked(!state.placement.locked) },
                )
                ParallaxChip(text = "Move it", selected = false, onClick = viewModel::reset)
            }
        }
    }
}

private fun sizeSummary(state: ArPlacementUiState): String {
    val (width, depth) = state.placement.footprintMetres
    val height = state.placement.heightMetres
    return if (state.imperialUnits) {
        val perMetre = 1f / FEET_TO_METRES
        "${round(width * perMetre)} × ${round(depth * perMetre)} × " +
            "${round(height * perMetre)} ft"
    } else {
        "${round(width)} × ${round(depth)} × ${round(height)} m"
    }
}

private fun round(value: Float): String =
    (kotlin.math.round(value * CENTIMETRE_PRECISION) / CENTIMETRE_PRECISION).toString()

/** Row-major to column-major, which is what GLES uniforms expect. */
private fun transpose(source: FloatArray, destination: FloatArray) {
    for (row in 0 until MATRIX_SIDE) {
        for (column in 0 until MATRIX_SIDE) {
            destination[column * MATRIX_SIDE + row] = source[row * MATRIX_SIDE + column]
        }
    }
}

private const val MATRIX_SIDE = 4
private const val MATRIX_SIZE = MATRIX_SIDE * MATRIX_SIDE
private const val NEAR_METRES = 0.05f
private const val FAR_METRES = 60f
private const val DEGREES_TO_RADIANS = 0.017453292f
private const val MAX_TRANSPARENCY = 0.7f
private const val FEET_TO_METRES = 0.3048f

/** Two decimal places, which at these sizes is the nearest centimetre. */
private const val CENTIMETRE_PRECISION = 100f
