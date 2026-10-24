package uk.co.jacoblevy.parallax.feature.capture.session

import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.media.Image
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.CameraIntrinsics
import uk.co.jacoblevy.parallax.core.model.CaptureFrame
import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.Pose
import uk.co.jacoblevy.parallax.core.model.TrackingState
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.min

/**
 * Writes captured frames to the project directory.
 *
 * Two formats, both chosen for what reads them:
 *
 * * Colour goes out as JPEG. A 160-frame scan is around 250 MB of JPEG against
 *   1.3 GB of PNG, and the conversion starts from the camera's own YUV planes so there is no
 *   decode step. The native engine has a baseline JPEG decoder for exactly this.
 * * Depth goes out as a raw float map -- `int32` width, `int32` height, then the samples in
 *   metres, then one confidence byte per sample. That is the format `readDepthRaw` in
 *   `Pipeline.cpp` parses, and it costs nothing to write.
 *
 * Nothing here touches the database. Frames accumulate in [frames] and the manifest is written
 * once at the end of the capture, so a session that is abandoned leaves files that
 * `ProjectStorage.deleteOrphans` will reclaim rather than a half-written manifest that would
 * make the whole capture unreadable.
 */
open class FrameRecorder(
    private val storage: ProjectStorage,
    private val projectId: String,
    private val jpegQuality: Int = DEFAULT_JPEG_QUALITY,
    private val maxColourDimension: Int = DEFAULT_MAX_DIMENSION,
) {
    private val recorded = mutableListOf<CaptureFrame>()

    val frames: List<CaptureFrame> get() = recorded
    val frameCount: Int get() = recorded.size

    /** Bytes written so far, for the live storage readout. */
    var bytesWritten: Long = 0
        private set

    fun prepare() {
        storage.create(projectId)
    }

    /**
     * Write one frame.
     *
     * @return the recorded frame, or null when there was no image or it could not be converted.
     *   A conversion failure means the camera handed over a format the converter does not know,
     *   which is reported rather than retried.
     */
    @Suppress("LongParameterList")
    open fun record(
        index: Int,
        image: Image?,
        depth: Image?,
        pose: Pose?,
        intrinsics: CameraIntrinsics,
        quality: FrameQuality,
        imageHash: Long,
        isKeyframe: Boolean,
        trackingState: TrackingState,
        timestampNanos: Long,
    ): CaptureFrame? {
        if (image == null) return null
        val imageName = "%06d.jpg".format(index)
        val jpeg = encodeJpeg(image) ?: return null
        File(storage.framesDir(projectId), imageName).writeBytes(jpeg)
        var written = jpeg.size.toLong()

        var depthName: String? = null
        if (depth != null) {
            val candidate = "%06d.depth".format(index)
            val depthFile = File(storage.depthDir(projectId), candidate)
            val depthBytes = writeDepth(depth, depthFile)
            if (depthBytes <= 0) {
                depthFile.delete()
            } else {
                depthName = candidate
                written += depthBytes
            }
        }

        // The intrinsics are recorded at the resolution the frame was *written* at, not the one
        // the camera reported. Scaling the image and leaving the focal length alone gives a
        // reconstruction that is subtly wrong everywhere, with nothing to point at.
        val (writtenWidth, writtenHeight) = scaledSize(image.width, image.height)

        return recordMetadata(
            index = index,
            imageName = imageName,
            depthName = depthName,
            pose = pose,
            intrinsics = intrinsics.scaledTo(writtenWidth, writtenHeight),
            quality = quality,
            imageHash = imageHash,
            isKeyframe = isKeyframe,
            trackingState = trackingState,
            timestampNanos = timestampNanos,
            bytes = written,
        )
    }

    /**
     * Append a frame's metadata to the manifest-in-progress.
     *
     * Separate from [record] so that the image writing and the bookkeeping can be exercised
     * independently: the capture loop's tests drive the bookkeeping without encoding JPEG,
     * which cannot run off a device.
     */
    @Suppress("LongParameterList")
    protected fun recordMetadata(
        index: Int,
        imageName: String,
        depthName: String?,
        pose: Pose?,
        intrinsics: CameraIntrinsics,
        quality: FrameQuality,
        imageHash: Long,
        isKeyframe: Boolean,
        trackingState: TrackingState,
        timestampNanos: Long,
        bytes: Long,
    ): CaptureFrame {
        val frame = CaptureFrame(
            index = index,
            timestampNanos = timestampNanos,
            imageFileName = imageName,
            depthFileName = depthName,
            pose = pose,
            intrinsics = intrinsics,
            quality = quality,
            isKeyframe = isKeyframe,
            imageHash = imageHash,
            trackingState = trackingState,
        )
        recorded += frame
        bytesWritten += bytes
        return frame
    }

    /** Discard everything written so far. Used when a capture is abandoned. */
    open fun discard() {
        recorded.clear()
        bytesWritten = 0
        storage.framesDir(projectId).deleteRecursively()
        storage.depthDir(projectId).deleteRecursively()
        storage.create(projectId)
    }

    // ---- conversion --------------------------------------------------------

    /**
     * The size a frame is written at.
     *
     * Capped because reconstruction gains nothing above a couple of megapixels: the TSDF voxel
     * size is the limit on detail, and feature matching is more reliable on a slightly
     * downscaled image than on a noisy full-resolution one. The cap is also what keeps a scan
     * under a gigabyte.
     */
    private fun scaledSize(width: Int, height: Int): Pair<Int, Int> {
        val longest = maxOf(width, height)
        if (longest <= maxColourDimension) return width to height
        val scale = maxColourDimension.toDouble() / longest
        // Even dimensions: an odd width breaks the 4:2:0 chroma subsampling the encoder uses.
        val scaledWidth = ((width * scale).toInt() / 2) * 2
        val scaledHeight = ((height * scale).toInt() / 2) * 2
        return scaledWidth.coerceAtLeast(2) to scaledHeight.coerceAtLeast(2)
    }

    /**
     * Convert a YUV_420_888 camera image to JPEG.
     *
     * `YuvImage` wants NV21, which is Y followed by interleaved V and U. The camera's planes are
     * usually already close to that, but the row and pixel strides are device-specific -- some
     * devices hand over semi-planar data with a pixel stride of 2 on the chroma planes, others
     * fully planar with a stride of 1 -- so the copy honours both rather than assuming.
     */
    private fun encodeJpeg(image: Image): ByteArray? {
        if (image.format != ImageFormat.YUV_420_888) return null
        val nv21 = toNv21(image) ?: return null
        val (targetWidth, targetHeight) = scaledSize(image.width, image.height)

        val yuvImage = YuvImage(nv21, ImageFormat.NV21, image.width, image.height, null)
        val output = java.io.ByteArrayOutputStream(nv21.size / JPEG_SIZE_DIVISOR)
        // compressToJpeg takes the crop rectangle, not a scale, so the downscale is done by
        // compressing the full frame and letting the engine work at that size. Cropping to a
        // smaller rectangle would throw away field of view, which is the opposite of what a
        // scanner wants.
        val cropped = Rect(0, 0, image.width, image.height)
        if (!yuvImage.compressToJpeg(cropped, jpegQuality, output)) return null
        val bytes = output.toByteArray()
        return if (targetWidth == image.width) bytes else downscaleJpeg(bytes, targetWidth, targetHeight)
    }

    /**
     * Downscale an encoded JPEG.
     *
     * Decode-and-re-encode rather than a clever DCT-domain scale: `BitmapFactory` can decode at
     * a power-of-two sample size in hardware, which is both faster and lower-peak-memory than
     * anything done by hand, and the re-encode is the same cost as the original.
     */
    private fun downscaleJpeg(bytes: ByteArray, targetWidth: Int, targetHeight: Int): ByteArray {
        val options = android.graphics.BitmapFactory.Options()
        val source = android.graphics.BitmapFactory.decodeByteArray(
            bytes,
            0,
            bytes.size,
            options,
        ) ?: return bytes
        val scaled = android.graphics.Bitmap.createScaledBitmap(
            source,
            targetWidth,
            targetHeight,
            true,
        )
        val output = java.io.ByteArrayOutputStream(bytes.size / 2)
        scaled.compress(android.graphics.Bitmap.CompressFormat.JPEG, jpegQuality, output)
        if (scaled !== source) scaled.recycle()
        source.recycle()
        return output.toByteArray()
    }

    private fun toNv21(image: Image): ByteArray? {
        val planes = image.planes
        if (planes.size < YUV_PLANES) return null
        val width = image.width
        val height = image.height
        // NV21 is one luma byte per pixel plus one interleaved chroma pair per 2x2 block,
        // i.e. 1.5 bytes per pixel.
        val result = ByteArray(width * height * NV21_NUMERATOR / NV21_DENOMINATOR)

        // --- luma ---
        copyPlane(
            source = planes[0].buffer,
            rowStride = planes[0].rowStride,
            pixelStride = planes[0].pixelStride,
            width = width,
            height = height,
            destination = result,
            destinationOffset = 0,
            destinationPixelStride = 1,
        )

        // --- chroma, interleaved as V then U ---
        val chromaWidth = width / 2
        val chromaHeight = height / 2
        val chromaOffset = width * height
        copyPlane(
            source = planes[2].buffer,
            rowStride = planes[2].rowStride,
            pixelStride = planes[2].pixelStride,
            width = chromaWidth,
            height = chromaHeight,
            destination = result,
            destinationOffset = chromaOffset,
            destinationPixelStride = 2,
        )
        copyPlane(
            source = planes[1].buffer,
            rowStride = planes[1].rowStride,
            pixelStride = planes[1].pixelStride,
            width = chromaWidth,
            height = chromaHeight,
            destination = result,
            destinationOffset = chromaOffset + 1,
            destinationPixelStride = 2,
        )
        return result
    }

    private fun copyPlane(
        source: ByteBuffer,
        rowStride: Int,
        pixelStride: Int,
        width: Int,
        height: Int,
        destination: ByteArray,
        destinationOffset: Int,
        destinationPixelStride: Int,
    ) {
        source.rewind()
        val row = ByteArray(rowStride)
        var write = destinationOffset
        for (y in 0 until height) {
            val available = min(rowStride, source.remaining())
            if (available <= 0) break
            source.get(row, 0, available)
            if (pixelStride == 1 && destinationPixelStride == 1) {
                // The fast path, and the common one for the luma plane.
                System.arraycopy(row, 0, destination, write, min(width, available))
            } else {
                var read = 0
                var offset = write
                var x = 0
                while (x < width && read < available) {
                    destination[offset] = row[read]
                    read += pixelStride
                    offset += destinationPixelStride
                    x++
                }
            }
            write += width * destinationPixelStride
        }
    }

    /**
     * Write a 16-bit ARCore depth image as the engine's raw float format.
     *
     * ARCore's depth is millimetres in the low 13 bits with a 3-bit confidence in the high
     * bits. Both are extracted here rather than later: the pipeline wants metres, and the
     * confidence is what lets fusion weight a sample it is unsure about.
     */
    private fun writeDepth(depth: Image, file: File): Int {
        val plane = depth.planes.firstOrNull() ?: return 0
        val width = depth.width
        val height = depth.height
        if (width <= 0 || height <= 0) return 0

        val samples = width * height
        val header = HEADER_BYTES
        val total = header + samples * Float.SIZE_BYTES + samples
        val out = ByteBuffer.allocate(total).order(ByteOrder.LITTLE_ENDIAN)
        out.putInt(width)
        out.putInt(height)

        val source = plane.buffer.order(ByteOrder.LITTLE_ENDIAN)
        val rowStride = plane.rowStride
        val confidences = ByteArray(samples)
        for (y in 0 until height) {
            for (x in 0 until width) {
                val offset = y * rowStride + x * Short.SIZE_BYTES
                if (offset + 1 >= source.limit()) {
                    out.putFloat(0f)
                    continue
                }
                val raw = source.getShort(offset).toInt() and UNSIGNED_SHORT_MASK
                val millimetres = raw and DEPTH_MILLIMETRE_MASK
                val confidence = (raw shr DEPTH_CONFIDENCE_SHIFT) and DEPTH_CONFIDENCE_MASK
                out.putFloat(millimetres / MILLIMETRES_PER_METRE)
                // Scale the 3-bit confidence to the full byte range the engine expects.
                confidences[y * width + x] =
                    (confidence * BYTE_MAX / DEPTH_CONFIDENCE_MASK).toByte()
            }
        }
        out.put(confidences)

        FileOutputStream(file).use { stream -> stream.write(out.array(), 0, out.position()) }
        return out.position()
    }

    companion object {
        const val DEFAULT_JPEG_QUALITY = 92

        /**
         * Longest side a frame is written at.
         *
         * Reconstruction detail is limited by the voxel size, not the image, and feature
         * matching is more reliable on a slightly downscaled frame than on a noisy
         * full-resolution one. This is also what keeps a Max-quality scan under a gigabyte.
         */
        const val DEFAULT_MAX_DIMENSION = 2_048

        private const val HEADER_BYTES = 8
        private const val YUV_PLANES = 3
        private const val NV21_NUMERATOR = 3
        private const val NV21_DENOMINATOR = 2
        private const val UNSIGNED_SHORT_MASK = 0xFFFF
        private const val JPEG_SIZE_DIVISOR = 8
        private const val MILLIMETRES_PER_METRE = 1_000f

        /** ARCore packs millimetres in the low 13 bits and confidence in the top 3. */
        private const val DEPTH_MILLIMETRE_MASK = 0x1FFF
        private const val DEPTH_CONFIDENCE_SHIFT = 13
        private const val DEPTH_CONFIDENCE_MASK = 0x7
        private const val BYTE_MAX = 255
    }
}
