package uk.co.jacoblevy.parallax.feature.viewer.ui

import android.view.SurfaceView
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import uk.co.jacoblevy.parallax.core.model.LightingPreset
import uk.co.jacoblevy.parallax.feature.viewer.ViewerUiState
import uk.co.jacoblevy.parallax.feature.viewer.render.ModelSurface
import java.io.File
import java.nio.ByteBuffer

/**
 * The model, on a [SurfaceView] driven by [ModelSurface].
 *
 * A SurfaceView rather than a TextureView: the model fills the screen and
 * never animates in or out, so there is nothing to gain from compositing it
 * into the view hierarchy and a frame per frame to lose.
 */
@Composable
internal fun ModelView(
    state: ViewerUiState,
    onRendererFailed: () -> Unit,
    onFrame: (seconds: Float) -> Unit,
    onViewportChanged: (width: Int, height: Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val latest = rememberUpdatedState(state)
    val frame = rememberUpdatedState(onFrame)
    val failed = rememberUpdatedState(onRendererFailed)
    val surface = remember { mutableStateOfSurface() }

    AndroidView(
        modifier = modifier,
        factory = { context ->
            SurfaceView(context).also { view ->
                val model = ModelSurface(view)
                model.source = object : ModelSurface.Source {
                    private var lastFrameNanos = 0L

                    override fun cameraPose(): ModelSurface.Pose {
                        val now = System.nanoTime()
                        if (lastFrameNanos != 0L) {
                            frame.value((now - lastFrameNanos) / NANOS_PER_SECOND)
                        }
                        lastFrameNanos = now
                        val current = latest.value
                        return if (current.walking) {
                            val walk = current.walk
                            ModelSurface.Pose(
                                eyeX = walk.eyeX, eyeY = walk.eyeY, eyeZ = walk.eyeZ,
                                targetX = walk.eyeX + walk.forwardX,
                                targetY = walk.eyeY + walk.forwardY,
                                targetZ = walk.eyeZ + walk.forwardZ,
                                upX = 0f, upY = 1f, upZ = 0f,
                            )
                        } else {
                            val orbit = current.orbit
                            ModelSurface.Pose(
                                eyeX = orbit.eyeX, eyeY = orbit.eyeY, eyeZ = orbit.eyeZ,
                                targetX = orbit.targetX,
                                targetY = orbit.targetY,
                                targetZ = orbit.targetZ,
                                upX = orbit.upX, upY = orbit.upY, upZ = orbit.upZ,
                            )
                        }
                    }

                    override fun fieldOfViewDegrees(): Float =
                        latest.value.orbit.fieldOfViewDegrees

                    override fun lighting(): LightingPreset = latest.value.lighting

                    override fun exposureStops(): Float =
                        latest.value.exposureStops + latest.value.editPlan.exposureStops
                }
                // The same report either way: a renderer that never started and one that
                // stopped mid-frame leave the user in the same place, and the screen already
                // knows how to say the 3D view is unavailable while keeping export working.
                model.onRendererFailure = { failed.value() }
                if (!model.start()) onRendererFailed()
                surface.value = model
                view.addOnLayoutChangeListener { _, left, top, right, bottom, _, _, _, _ ->
                    onViewportChanged(right - left, bottom - top)
                }
            }
        },
        onRelease = {
            surface.value?.destroy()
            surface.value = null
        },
    )

    // The loaded file is what changes when the render mode changes, so this is
    // what swaps the geometry rather than any explicit call from the ViewModel.
    LaunchedEffect(state.modelFile) {
        val model = surface.value ?: return@LaunchedEffect
        val file = state.modelFile
        if (file == null) {
            model.clearModel()
        } else if (!model.loadModel(file.readIntoDirectBuffer())) {
            onRendererFailed()
        }
    }

    DisposableEffect(Unit) {
        surface.value?.resume()
        onDispose { surface.value?.pause() }
    }
}

/**
 * Filament needs a direct buffer: it reads the GLB from native code, and a
 * heap array would have to be copied across the boundary anyway.
 */
private fun File.readIntoDirectBuffer(): ByteBuffer {
    val bytes = readBytes()
    return ByteBuffer.allocateDirect(bytes.size).apply {
        put(bytes)
        rewind()
    }
}

private fun mutableStateOfSurface() = androidx.compose.runtime.mutableStateOf<ModelSurface?>(null)

private const val NANOS_PER_SECOND = 1_000_000_000f
