package uk.co.jacoblevy.parallax.feature.viewer.render

import android.util.Log
import android.view.Choreographer
import android.view.Surface
import android.view.SurfaceView
import com.google.android.filament.Camera
import com.google.android.filament.Colors
import com.google.android.filament.Engine
import com.google.android.filament.EntityManager
import com.google.android.filament.Fence
import com.google.android.filament.LightManager
import com.google.android.filament.Renderer
import com.google.android.filament.Scene
import com.google.android.filament.Skybox
import com.google.android.filament.SwapChain
import com.google.android.filament.View
import com.google.android.filament.Viewport
import com.google.android.filament.android.DisplayHelper
import com.google.android.filament.android.UiHelper
import com.google.android.filament.gltfio.AssetLoader
import com.google.android.filament.gltfio.FilamentAsset
import com.google.android.filament.gltfio.ResourceLoader
import com.google.android.filament.gltfio.UbershaderProvider
import com.google.android.filament.utils.Utils
import uk.co.jacoblevy.parallax.core.model.LightingPreset
import java.nio.ByteBuffer

/**
 * The only file in the viewer that knows Filament exists.
 *
 * Everything above it works in terms of a camera position and a GLB on disk,
 * which is what makes the rest of the viewer -- the camera feel, the picking,
 * the measurements, the edits -- testable on a JVM. This layer is kept
 * deliberately small and dull for the same reason the ARCore layer was: it
 * cannot be exercised without a GPU, so there should be as little of it as
 * possible and nothing clever in it.
 *
 * Initialisation is allowed to fail. A device whose driver refuses to give us
 * a context should still show the model's measurements and let it be exported,
 * so [start] reports failure rather than throwing and the screen degrades.
 */
class ModelSurface(private val surfaceView: SurfaceView) {

    /** Where the camera is, as the renderer needs it. */
    data class Pose(
        val eyeX: Float, val eyeY: Float, val eyeZ: Float,
        val targetX: Float, val targetY: Float, val targetZ: Float,
        val upX: Float, val upY: Float, val upZ: Float,
    )

    /** What the renderer wants to know each frame. */
    interface Source {
        fun cameraPose(): Pose
        fun fieldOfViewDegrees(): Float
        fun lighting(): LightingPreset
        /** Exposure compensation in stops, from the viewer's texture edits. */
        fun exposureStops(): Float
    }

    var source: Source? = null

    private var engine: Engine? = null
    private var renderer: Renderer? = null
    private var scene: Scene? = null
    private var view: View? = null
    private var camera: Camera? = null
    private var swapChain: SwapChain? = null
    private var assetLoader: AssetLoader? = null
    private var resourceLoader: ResourceLoader? = null
    private var materialProvider: UbershaderProvider? = null
    private var asset: FilamentAsset? = null
    private val lights = mutableListOf<Int>()
    private var appliedLighting: LightingPreset? = null

    private val uiHelper = UiHelper(UiHelper.ContextErrorPolicy.DONT_CHECK)
    private var displayHelper: DisplayHelper? = null
    private val choreographer: Choreographer = Choreographer.getInstance()
    private var running = false

    /**
     * Called when the renderer gives up, so the screen can say so.
     *
     * Set by the caller. Null means the failure is only logged, which is the right behaviour
     * for a surface being torn down and the wrong one for a viewer the user is looking at.
     */
    var onRendererFailure: ((String) -> Unit)? = null

    private var fatal = false

    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!running || fatal) return
            choreographer.postFrameCallback(this)
            // Filament is a native renderer behind a GPU driver: a lost context, an unsupported
            // format or a vendor bug surfaces here as something no signature declares. Drawing
            // nothing and saying so beats taking the app down mid-frame, and the model is still
            // on disk and still exportable, which is most of what the viewer was for.
            try {
                drawFrame()
            } catch (@Suppress("TooGenericExceptionCaught") unexpected: Throwable) {
                fatal = true
                running = false
                Log.e(TAG, "the model renderer failed", unexpected)
                onRendererFailure?.invoke("The 3D view stopped working on this device.")
            }
        }
    }

    /** True once a context exists; false means the screen should say so. */
    fun start(): Boolean = runCatching {
        Utils.init()
        val createdEngine = Engine.create()
        engine = createdEngine
        renderer = createdEngine.createRenderer()
        scene = createdEngine.createScene()
        val entity = EntityManager.get().create()
        camera = createdEngine.createCamera(entity)
        view = createdEngine.createView().apply {
            this.scene = scene
            this.camera = camera
            // Antialiasing on, bloom off: a scan has no emissive surfaces for
            // bloom to find, and it costs a chunk of a phone's frame budget.
            antiAliasing = View.AntiAliasing.FXAA
            blendMode = View.BlendMode.OPAQUE
        }
        materialProvider = UbershaderProvider(createdEngine)
        assetLoader = AssetLoader(createdEngine, materialProvider!!, EntityManager.get())
        resourceLoader = ResourceLoader(createdEngine)

        displayHelper = DisplayHelper(surfaceView.context)
        uiHelper.renderCallback = SurfaceCallback()
        uiHelper.attachTo(surfaceView)
        true
    }.getOrElse { false }

    fun resume() {
        if (engine == null || running) return
        running = true
        choreographer.postFrameCallback(frameCallback)
    }

    fun pause() {
        running = false
        choreographer.removeFrameCallback(frameCallback)
    }

    fun destroy() {
        pause()
        val currentEngine = engine ?: return
        // Filament's teardown order is not optional: anything that still
        // references the engine has to go before the engine does, and a fence
        // is the only way to know the GPU is finished with it.
        clearModel()
        uiHelper.detach()
        lights.forEach { currentEngine.destroyEntity(it) }
        lights.clear()
        resourceLoader?.destroy()
        assetLoader?.destroy()
        materialProvider?.destroyMaterials()
        view?.let { currentEngine.destroyView(it) }
        scene?.let { currentEngine.destroyScene(it) }
        camera?.let { currentEngine.destroyCameraComponent(it.entity) }
        currentEngine.flushAndWait()
        currentEngine.destroy()
        engine = null
        renderer = null
        scene = null
        view = null
        camera = null
    }

    /**
     * Replace what is on screen with the GLB in [glb].
     *
     * One asset at a time: the viewer shows one model, and switching render
     * mode swaps the file rather than toggling visibility, so there is never a
     * second copy of a few hundred thousand triangles in memory.
     */
    fun loadModel(glb: ByteBuffer): Boolean {
        if (engine == null) return false
        val loader = assetLoader ?: return false
        return runCatching {
            clearModel()
            val loaded = loader.createAsset(glb) ?: return false
            resourceLoader?.loadResources(loaded)
            loaded.releaseSourceData()
            scene?.addEntities(loaded.entities)
            asset = loaded
            true
        }.getOrElse { false }
    }

    fun clearModel() {
        if (engine == null) return
        asset?.let {
            scene?.removeEntities(it.entities)
            assetLoader?.destroyAsset(it)
        }
        asset = null
    }

    private fun drawFrame() {
        val currentRenderer = renderer ?: return
        val currentView = view ?: return
        val currentSwapChain = swapChain ?: return
        val currentCamera = camera ?: return
        val currentSource = source ?: return

        val pose = currentSource.cameraPose()
        currentCamera.lookAt(
            pose.eyeX.toDouble(), pose.eyeY.toDouble(), pose.eyeZ.toDouble(),
            pose.targetX.toDouble(), pose.targetY.toDouble(), pose.targetZ.toDouble(),
            pose.upX.toDouble(), pose.upY.toDouble(), pose.upZ.toDouble(),
        )
        val viewport = currentView.viewport
        if (viewport.width > 0 && viewport.height > 0) {
            currentCamera.setProjection(
                currentSource.fieldOfViewDegrees().toDouble(),
                viewport.width.toDouble() / viewport.height.toDouble(),
                NEAR_PLANE_METRES,
                FAR_PLANE_METRES,
                Camera.Fov.VERTICAL,
            )
        }
        // Filament's camera is a physical one, so "brightness" is aperture and
        // shutter rather than a multiplier; holding two fixed and moving the
        // sensitivity is the one that behaves like an exposure slider.
        val sensitivity = BASE_SENSITIVITY * Math.pow(2.0, currentSource.exposureStops().toDouble())
        currentCamera.setExposure(APERTURE, SHUTTER_SPEED, sensitivity.toFloat())

        applyLighting(currentSource.lighting())

        if (currentRenderer.beginFrame(currentSwapChain, System.nanoTime())) {
            currentRenderer.render(currentView)
            currentRenderer.endFrame()
        }
    }

    /**
     * Lighting presets as directional lights rather than an environment map.
     *
     * A real image-based light needs an HDR cubemap processed by Filament's
     * `cmgen` at build time, and a bundled one would add several megabytes to
     * an app whose whole point is that it needs no downloads. Three
     * directional lights with different intensities and colour temperatures
     * give four presets that genuinely look different, which is what the
     * setting is for. See docs/DECISIONS.md (ADR-014).
     */
    private fun applyLighting(preset: LightingPreset) {
        if (preset == appliedLighting) return
        val currentEngine = engine ?: return
        val currentScene = scene ?: return
        if (lights.isNotEmpty()) {
            currentScene.removeEntities(lights.toIntArray())
            lights.forEach { currentEngine.destroyEntity(it) }
            lights.clear()
        }

        val setup = when (preset) {
            // A bright key from above and in front, a dim cool fill from the
            // opposite side, and a rim light to separate the model from the
            // background: the standard three-point setup.
            LightingPreset.Studio -> listOf(
                Lamp(lux = 100_000f, kelvin = 5600f, x = -0.5f, y = -1f, z = -0.6f),
                Lamp(lux = 30_000f, kelvin = 7000f, x = 0.8f, y = -0.3f, z = 0.4f),
                Lamp(lux = 40_000f, kelvin = 6500f, x = 0.2f, y = -0.2f, z = 1f),
            )
            // Flat, even, from slightly above: shows the surface rather than
            // the lighting, which is what you want when judging a scan.
            LightingPreset.Neutral -> listOf(
                Lamp(lux = 70_000f, kelvin = 6500f, x = -0.2f, y = -1f, z = -0.3f),
                Lamp(lux = 45_000f, kelvin = 6500f, x = 0.3f, y = -0.6f, z = 0.5f),
            )
            // Warm overhead against a cool bounce from below, which is what
            // daylight in a room actually does.
            LightingPreset.Environment -> listOf(
                Lamp(lux = 85_000f, kelvin = 5200f, x = -0.3f, y = -1f, z = -0.2f),
                Lamp(lux = 35_000f, kelvin = 9000f, x = 0.2f, y = 0.7f, z = 0.3f),
                Lamp(lux = 25_000f, kelvin = 7500f, x = 1f, y = -0.2f, z = -0.6f),
            )
            // One light straight down the view axis: no shading at all, so
            // the model's own colour is all that is left.
            LightingPreset.Flat -> listOf(Lamp(lux = 120_000f, kelvin = 6500f, x = 0f, y = -0.15f, z = -1f))
        }

        val colour = FloatArray(RGB_CHANNELS)
        for (lamp in setup) {
            Colors.cct(lamp.kelvin, colour)
            val entity = EntityManager.get().create()
            LightManager.Builder(LightManager.Type.DIRECTIONAL)
                .color(colour[RED], colour[GREEN], colour[BLUE])
                .intensity(lamp.lux)
                .direction(lamp.x, lamp.y, lamp.z)
                .castShadows(lamp == setup.first() && preset != LightingPreset.Flat)
                .build(currentEngine, entity)
            currentScene.addEntity(entity)
            lights.add(entity)
        }
        // A plain dark skybox rather than transparency: a scan with holes in
        // it reads much better against something than against nothing.
        if (currentScene.skybox == null) {
            currentScene.skybox = Skybox.Builder()
                .color(BACKGROUND[RED], BACKGROUND[GREEN], BACKGROUND[BLUE], 1f)
                .build(currentEngine)
        }
        appliedLighting = preset
    }

    /** One directional light: how bright, how warm, and which way it points. */
    private data class Lamp(
        val lux: Float,
        val kelvin: Float,
        val x: Float,
        val y: Float,
        val z: Float,
    )

    private inner class SurfaceCallback : UiHelper.RendererCallback {
        override fun onNativeWindowChanged(surface: Surface) {
            val currentEngine = engine ?: return
            swapChain?.let { currentEngine.destroySwapChain(it) }
            swapChain = currentEngine.createSwapChain(surface)
            renderer?.let { displayHelper?.attach(it, surfaceView.display) }
        }

        override fun onDetachedFromSurface() {
            val currentEngine = engine ?: return
            displayHelper?.detach()
            swapChain?.let {
                currentEngine.destroySwapChain(it)
                // Without the fence the swap chain can be freed while the GPU
                // is still reading from it, which on some drivers is a crash
                // in the compositor rather than in this process.
                currentEngine.createFence().apply {
                    Fence.waitAndDestroy(this, Fence.Mode.FLUSH)
                }
                swapChain = null
            }
        }

        override fun onResized(width: Int, height: Int) {
            view?.viewport = Viewport(0, 0, width, height)
        }
    }

    private companion object {
        const val NEAR_PLANE_METRES = 0.02
        const val FAR_PLANE_METRES = 200.0

        /**
         * A fixed photographic exposure, chosen so that a model lit by the
         * presets above lands mid-grey. The user's exposure slider moves
         * sensitivity around this.
         */
        const val APERTURE = 16f
        const val SHUTTER_SPEED = 1f / 125f
        const val BASE_SENSITIVITY = 100.0

        const val RGB_CHANNELS = 3
        const val RED = 0
        const val GREEN = 1
        const val BLUE = 2

        val BACKGROUND = floatArrayOf(0.043f, 0.047f, 0.055f)

        const val TAG = "ModelSurface"
    }
}
