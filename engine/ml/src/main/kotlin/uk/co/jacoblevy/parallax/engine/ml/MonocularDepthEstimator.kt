package uk.co.jacoblevy.parallax.engine.ml

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import org.tensorflow.lite.Interpreter
import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.inject.Inject
import javax.inject.Singleton

/** A relative-depth prediction, in the network's own arbitrary units. */
data class RelativeDepth(val values: FloatArray, val width: Int, val height: Int) {
    override fun equals(other: Any?): Boolean = this === other ||
        (
            other is RelativeDepth && width == other.width && height == other.height &&
                values.contentEquals(other.values)
            )

    override fun hashCode(): Int = (width * 31 + height) * 31 + values.contentHashCode()
}

/** Monocular depth, where the model is installed. */
interface DepthEstimator : Closeable {
    val isAvailable: Boolean
    fun estimate(image: Bitmap): RelativeDepth?
}

/**
 * Runs the monocular depth model through LiteRT.
 *
 * The interpreter is created lazily and held: initialising it costs tens of milliseconds and
 * the model runs once per keyframe, so creating one per call would dominate the cost. It is
 * **not** thread-safe -- LiteRT interpreters are not -- so callers serialise on the
 * reconstruction dispatcher, which is single-threaded for exactly this kind of resource.
 *
 * Inference runs on the CPU through XNNPACK. LiteRT 2.3.0's `litert-gpu` artifact ships native
 * libraries but no public Java entry point for them -- its `GpuDelegateProxy` is
 * package-private, and the newer `CompiledModel` accelerator API is not in these artifacts --
 * so there is nothing to call. The trade-off is recorded in `docs/DECISIONS.md`; it costs
 * inference speed on a model that is an optional enhancement in the first place, since depth
 * comes from multi-view stereo by default.
 */
@Singleton
class LiteRtDepthEstimator @Inject constructor(
    @ApplicationContext private val context: Context,
    private val registry: MlModelRegistry,
) : DepthEstimator {

    override val isAvailable: Boolean get() = registry.isAvailable(MlModel.MonocularDepth)

    private var interpreter: Interpreter? = null
    private var inputWidth = 0
    private var inputHeight = 0
    private var failed = false

    override fun estimate(image: Bitmap): RelativeDepth? {
        if (!isAvailable || failed) return null
        val runner = interpreter ?: createInterpreter() ?: return null

        val input = toInput(image, inputWidth, inputHeight)
        val output = ByteBuffer
            .allocateDirect(inputWidth * inputHeight * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
        return try {
            runner.run(input, output)
            output.rewind()
            val values = FloatArray(inputWidth * inputHeight)
            output.asFloatBuffer().get(values)
            RelativeDepth(values, inputWidth, inputHeight)
        } catch (failure: RuntimeException) {
            // One failure is treated as permanent for this process: the usual cause is a
            // delegate that reports support and then cannot run the graph, and retrying it
            // once per keyframe would make a long reconstruction slower than no model at all.
            Log.w(TAG, "monocular depth inference failed; falling back to stereo depth", failure)
            failed = true
            close()
            null
        }
    }

    private fun createInterpreter(): Interpreter? = runCatching {
        val model = context.assets.openFd(MlModel.MonocularDepth.assetPath).use { descriptor ->
            descriptor.createInputStream().use { stream ->
                val bytes = stream.readBytes()
                ByteBuffer.allocateDirect(bytes.size).order(ByteOrder.nativeOrder()).apply {
                    put(bytes)
                    rewind()
                }
            }
        }

        val options = Interpreter.Options().apply {
            numThreads = Runtime.getRuntime().availableProcessors().coerceAtMost(MAX_THREADS)
            // XNNPACK is a large win on the depthwise convolutions this network is mostly made
            // of, and it is the fastest path LiteRT 2.3.0 actually exposes.
            setUseXNNPACK(true)
        }
        val created = Interpreter(model, options)
        val shape = created.getInputTensor(0).shape()
        // NHWC, so height then width. Reading the shape from the model rather than assuming
        // 518x518 means a different export of the same network works unchanged.
        inputHeight = shape.getOrElse(1) { DEFAULT_INPUT }
        inputWidth = shape.getOrElse(2) { DEFAULT_INPUT }
        interpreter = created
        created
    }.getOrElse { failure ->
        Log.w(TAG, "could not load the monocular depth model", failure)
        failed = true
        null
    }

    /**
     * Resize to the model's input and normalise.
     *
     * ImageNet mean and standard deviation, which is what the Depth Anything family was trained
     * with. Feeding raw 0..1 values instead produces output that looks plausible and is wrong
     * in a way no test of the plumbing would catch.
     */
    private fun toInput(image: Bitmap, width: Int, height: Int): ByteBuffer {
        val scaled = if (image.width == width && image.height == height) {
            image
        } else {
            Bitmap.createScaledBitmap(image, width, height, true)
        }
        val buffer = ByteBuffer
            .allocateDirect(width * height * CHANNELS * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
        val pixels = IntArray(width * height)
        scaled.getPixels(pixels, 0, width, 0, 0, width, height)
        for (pixel in pixels) {
            val r = ((pixel shr RED_SHIFT) and BYTE_MASK) / BYTE_SCALE
            val g = ((pixel shr GREEN_SHIFT) and BYTE_MASK) / BYTE_SCALE
            val b = (pixel and BYTE_MASK) / BYTE_SCALE
            buffer.putFloat((r - MEAN_R) / STD_R)
            buffer.putFloat((g - MEAN_G) / STD_G)
            buffer.putFloat((b - MEAN_B) / STD_B)
        }
        buffer.rewind()
        if (scaled !== image) scaled.recycle()
        return buffer
    }

    override fun close() {
        runCatching { interpreter?.close() }
        interpreter = null
    }

    private companion object {
        const val TAG = "LiteRtDepth"
        const val CHANNELS = 3
        const val MAX_THREADS = 4
        const val DEFAULT_INPUT = 518

        const val RED_SHIFT = 16
        const val GREEN_SHIFT = 8
        const val BYTE_MASK = 0xFF
        const val BYTE_SCALE = 255f

        const val MEAN_R = 0.485f
        const val MEAN_G = 0.456f
        const val MEAN_B = 0.406f
        const val STD_R = 0.229f
        const val STD_G = 0.224f
        const val STD_B = 0.225f
    }
}
