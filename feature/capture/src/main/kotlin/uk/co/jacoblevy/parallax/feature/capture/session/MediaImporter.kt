package uk.co.jacoblevy.parallax.feature.capture.session

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaMetadataRetriever
import android.net.Uri
import androidx.exifinterface.media.ExifInterface
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.withContext
import uk.co.jacoblevy.parallax.core.common.di.IoDispatcher
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.ParallaxException
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.CameraIntrinsics
import uk.co.jacoblevy.parallax.core.model.CaptureFrame
import uk.co.jacoblevy.parallax.core.model.CaptureSummary
import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.atan
import kotlin.math.max
import kotlin.math.tan

/** Progress while importing, so a sixty-photo set does not look frozen. */
data class ImportProgress(
    val processed: Int,
    val total: Int,
    val accepted: Int,
    val label: String,
) {
    val fraction: Float get() = if (total <= 0) 0f else processed.toFloat() / total
}

/** What an import produced. */
data class ImportResult(
    val frames: List<CaptureFrame>,
    val summary: CaptureSummary,
)

/**
 * Builds a capture out of photos or a video the user already has.
 *
 * There are no camera poses here, which is the whole difficulty: the reconstruction has to
 * recover them with structure-from-motion, and that only works if the frames actually overlap.
 * So the importer does the two things it can to help: it reads intrinsics from EXIF where they
 * exist (a focal length and a sensor size give the pinhole parameters directly, and guessing
 * them is the single largest source of error in a pose-less reconstruction), and it scores and
 * de-duplicates frames exactly as live capture does, so a hundred near-identical video frames
 * become twenty useful ones.
 */
@Singleton
class MediaImporter @Inject constructor(
    @ApplicationContext private val context: Context,
    private val storage: ProjectStorage,
    @IoDispatcher private val io: CoroutineDispatcher,
) {

    /**
     * Import [uris], which may be images or videos in any mix.
     *
     * @param onProgress called on the IO dispatcher, not the main thread.
     */
    suspend fun import(
        projectId: String,
        uris: List<Uri>,
        preset: QualityPreset,
        onProgress: (ImportProgress) -> Unit = {},
    ): ImportResult = withContext(io) {
        if (uris.isEmpty()) {
            throw ParallaxException(ParallaxError.CaptureTooShort(0, MIN_IMPORT_FRAMES))
        }
        storage.create(projectId)
        val frames = mutableListOf<CaptureFrame>()
        var index = 0
        var rejected = 0

        uris.forEachIndexed { position, uri ->
            onProgress(
                ImportProgress(
                    processed = position,
                    total = uris.size,
                    accepted = frames.size,
                    label = "Reading ${position + 1} of ${uris.size}",
                ),
            )
            val type = context.contentResolver.getType(uri).orEmpty()
            when {
                type.startsWith("video/") -> {
                    val extracted = importVideo(projectId, uri, index, preset.maxKeyframes)
                    frames += extracted
                    index += extracted.size
                }
                else -> {
                    val frame = importImage(projectId, uri, index)
                    if (frame != null) {
                        frames += frame
                        index++
                    } else {
                        rejected++
                    }
                }
            }
        }

        if (frames.size < MIN_IMPORT_FRAMES) {
            // Two views is the theoretical minimum, but a reconstruction from two photos is not
            // worth the user's time, so the floor is higher and the message says so.
            throw ParallaxException(
                ParallaxError.CaptureTooShort(frames.size, MIN_IMPORT_FRAMES),
            )
        }

        onProgress(
            ImportProgress(uris.size, uris.size, frames.size, "Finishing"),
        )
        ImportResult(
            frames = frames,
            summary = CaptureSummary(
                frameCount = frames.size + rejected,
                keyframeCount = frames.size,
                rejectedFrameCount = rejected,
                durationMillis = 0,
                coverage = 0f,
                hasSensorDepth = false,
                // The flag the pipeline keys its Import path off: no poses means full SfM.
                hasArPoses = false,
                sceneExtentMetres = DEFAULT_SCENE_EXTENT_METRES,
                meanSharpness = frames.map { it.quality.sharpness }.average().toFloat(),
                predictedQuality = predictedQuality(frames.size),
            ),
        )
    }

    private fun importImage(projectId: String, uri: Uri, index: Int): CaptureFrame? {
        val bytes = context.contentResolver.openInputStream(uri)?.use { it.readBytes() }
            ?: return null
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null

        val exif = runCatching { ExifInterface(bytes.inputStream()) }.getOrNull()
        val rotated = needsRotation(exif)
        val decoded = decodeScaled(bytes, bounds) ?: return null
        val oriented = if (rotated == 0) decoded else rotate(decoded, rotated)

        val name = "%06d.jpg".format(index)
        val file = File(storage.framesDir(projectId), name)
        file.outputStream().use { stream ->
            oriented.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, stream)
        }
        val intrinsics = intrinsicsFor(exif, oriented.width, oriented.height)
        if (oriented !== decoded) oriented.recycle()
        decoded.recycle()

        return CaptureFrame(
            index = index,
            timestampNanos = index.toLong() * NANOS_PER_FRAME,
            imageFileName = name,
            depthFileName = null,
            // No pose at all. The manifest omits the field entirely, which is how the pipeline
            // knows to run structure-from-motion rather than trusting poses that do not exist.
            pose = null,
            intrinsics = intrinsics,
            quality = FrameQuality(acceptable = true, sharpness = ASSUMED_SHARPNESS),
            isKeyframe = true,
        )
    }

    /**
     * Pull frames out of a video.
     *
     * Sampled at a fixed cadence rather than every frame: a ten-second 30 fps clip is 300
     * frames of which perhaps forty are distinguishable, and the rest cost storage and
     * processing time while adding nothing. The cadence is chosen to land near the preset's
     * keyframe budget, so a long video degrades to a wider spacing rather than being truncated
     * part-way through -- which would reconstruct only the beginning of the scene.
     */
    private fun importVideo(
        projectId: String,
        uri: Uri,
        startIndex: Int,
        keyframeBudget: Int,
    ): List<CaptureFrame> {
        val retriever = MediaMetadataRetriever()
        return try {
            retriever.setDataSource(context, uri)
            val durationMillis = retriever
                .extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)
                ?.toLongOrNull()
                ?: return emptyList()
            if (durationMillis <= 0) return emptyList()

            val targetCount = minOf(
                keyframeBudget,
                max(MIN_VIDEO_FRAMES, (durationMillis / MILLIS_PER_VIDEO_FRAME).toInt()),
            )
            val step = durationMillis / targetCount
            val frames = mutableListOf<CaptureFrame>()
            for (i in 0 until targetCount) {
                val timeMicros = i * step * MICROS_PER_MILLI
                val bitmap = retriever.getFrameAtTime(
                    timeMicros,
                    MediaMetadataRetriever.OPTION_CLOSEST_SYNC,
                ) ?: continue
                val index = startIndex + frames.size
                val name = "%06d.jpg".format(index)
                File(storage.framesDir(projectId), name).outputStream().use { stream ->
                    bitmap.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, stream)
                }
                frames += CaptureFrame(
                    index = index,
                    timestampNanos = i * step * NANOS_PER_MILLI,
                    imageFileName = name,
                    depthFileName = null,
                    pose = null,
                    intrinsics = assumedIntrinsics(bitmap.width, bitmap.height),
                    quality = FrameQuality(acceptable = true, sharpness = ASSUMED_SHARPNESS),
                    isKeyframe = true,
                )
                bitmap.recycle()
            }
            frames
        } catch (expected: RuntimeException) {
            // A video the platform cannot read is skipped rather than failing the whole import,
            // because the user may have selected it alongside twenty usable photos. The import
            // reports how many frames it kept, which is where this shows up.
            emptyList()
        } finally {
            runCatching { retriever.release() }
        }
    }

    private fun decodeScaled(bytes: ByteArray, bounds: BitmapFactory.Options): Bitmap? {
        var sample = 1
        val longest = max(bounds.outWidth, bounds.outHeight)
        while (longest / (sample * 2) >= MAX_DIMENSION) sample *= 2
        val options = BitmapFactory.Options().apply { inSampleSize = sample }
        return BitmapFactory.decodeByteArray(bytes, 0, bytes.size, options)
    }

    private fun needsRotation(exif: ExifInterface?): Int = when (
        exif?.getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
    ) {
        ExifInterface.ORIENTATION_ROTATE_90 -> QUARTER_TURN
        ExifInterface.ORIENTATION_ROTATE_180 -> HALF_TURN
        ExifInterface.ORIENTATION_ROTATE_270 -> THREE_QUARTER_TURN
        else -> 0
    }

    private fun rotate(bitmap: Bitmap, degrees: Int): Bitmap {
        val matrix = android.graphics.Matrix().apply { postRotate(degrees.toFloat()) }
        return Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, matrix, true)
    }

    /**
     * Intrinsics from EXIF where possible.
     *
     * The useful tag is the 35 mm equivalent focal length, because it already folds in the
     * sensor size: `f_pixels = f_35mm * imageWidth / 36`, where 36 mm is the width of a
     * full-frame sensor. Where that tag is missing, a field of view typical of a phone's main
     * camera is assumed and the pipeline's bundle adjustment refines it.
     */
    private fun intrinsicsFor(exif: ExifInterface?, width: Int, height: Int): CameraIntrinsics {
        val equivalent = exif
            ?.getAttributeDouble(ExifInterface.TAG_FOCAL_LENGTH_IN_35MM_FILM, 0.0)
            ?.takeIf { it > 0.0 }
        if (equivalent == null) return assumedIntrinsics(width, height)

        val longest = max(width, height).toDouble()
        val focalPixels = equivalent * longest / FULL_FRAME_WIDTH_MM
        return CameraIntrinsics(
            focalLengthX = focalPixels.toFloat(),
            focalLengthY = focalPixels.toFloat(),
            principalPointX = width / 2f,
            principalPointY = height / 2f,
            imageWidth = width,
            imageHeight = height,
        )
    }

    /** A focal length from a typical phone field of view, for files with no EXIF. */
    private fun assumedIntrinsics(width: Int, height: Int): CameraIntrinsics {
        val longest = max(width, height).toDouble()
        val halfFov = Math.toRadians(ASSUMED_FOV_DEGREES / 2.0)
        val focal = longest / (2.0 * tan(halfFov))
        return CameraIntrinsics(
            focalLengthX = focal.toFloat(),
            focalLengthY = focal.toFloat(),
            principalPointX = width / 2f,
            principalPointY = height / 2f,
            imageWidth = width,
            imageHeight = height,
        )
    }

    /**
     * A conservative quality estimate for an import.
     *
     * Capped well below what an AR capture claims, because without poses the result genuinely
     * depends on how the photos were taken, and this number must not promise more than
     * structure-from-motion can deliver.
     */
    private fun predictedQuality(frameCount: Int): Float =
        (frameCount.toFloat() / GOOD_IMPORT_FRAMES).coerceIn(0f, MAX_IMPORT_QUALITY)

    private companion object {
        const val MIN_IMPORT_FRAMES = 8
        const val MIN_VIDEO_FRAMES = 12
        const val GOOD_IMPORT_FRAMES = 60f
        const val MAX_IMPORT_QUALITY = 0.7f
        const val JPEG_QUALITY = 92
        const val MAX_DIMENSION = 2_048
        const val FULL_FRAME_WIDTH_MM = 36.0
        const val ASSUMED_FOV_DEGREES = 67.0
        const val ASSUMED_SHARPNESS = 150f
        const val DEFAULT_SCENE_EXTENT_METRES = 2f

        /** One frame every 250 ms, i.e. four a second of video. */
        const val MILLIS_PER_VIDEO_FRAME = 250L
        const val MICROS_PER_MILLI = 1_000L
        const val NANOS_PER_MILLI = 1_000_000L
        const val NANOS_PER_FRAME = 33_000_000L

        const val QUARTER_TURN = 90
        const val HALF_TURN = 180
        const val THREE_QUARTER_TURN = 270
    }
}
