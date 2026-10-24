package uk.co.jacoblevy.parallax.core.model

import kotlinx.serialization.Serializable

/** Pinhole intrinsics for one camera configuration. */
@Serializable
data class CameraIntrinsics(
    val focalLengthX: Float,
    val focalLengthY: Float,
    val principalPointX: Float,
    val principalPointY: Float,
    val imageWidth: Int,
    val imageHeight: Int,
    val distortionK1: Float = 0f,
    val distortionK2: Float = 0f,
) {
    val isValid: Boolean
        get() = focalLengthX > 0f && focalLengthY > 0f && imageWidth > 0 && imageHeight > 0

    /** Horizontal field of view in degrees. */
    val horizontalFovDegrees: Float
        get() = if (isValid) {
            Math.toDegrees(2.0 * kotlin.math.atan(imageWidth / (2.0 * focalLengthX))).toFloat()
        } else {
            0f
        }

    /** Rescale for a different image size, which happens whenever depth and colour differ. */
    fun scaledTo(width: Int, height: Int): CameraIntrinsics {
        if (!isValid || width <= 0 || height <= 0) return this
        val sx = width.toFloat() / imageWidth
        val sy = height.toFloat() / imageHeight
        return copy(
            focalLengthX = focalLengthX * sx,
            focalLengthY = focalLengthY * sy,
            principalPointX = principalPointX * sx,
            principalPointY = principalPointY * sy,
            imageWidth = width,
            imageHeight = height,
        )
    }
}

/**
 * A camera pose, stored as a world-from-camera 4x4 in column-major order.
 *
 * Column-major because that is what ARCore, OpenGL and Filament all use; converting at the
 * boundary once is far less error-prone than converting at every call site. The native engine
 * takes the same layout and inverts it internally to its camera-from-world convention.
 */
@Serializable
data class Pose(val matrix: FloatArray) {
    init {
        require(matrix.size == MATRIX_ELEMENTS) {
            "a pose matrix must have $MATRIX_ELEMENTS elements, got ${matrix.size}"
        }
    }

    val translationX: Float get() = matrix[12]
    val translationY: Float get() = matrix[13]
    val translationZ: Float get() = matrix[14]

    /** Distance between two camera centres, in metres. */
    fun distanceTo(other: Pose): Float {
        val dx = translationX - other.translationX
        val dy = translationY - other.translationY
        val dz = translationZ - other.translationZ
        return kotlin.math.sqrt(dx * dx + dy * dy + dz * dz)
    }

    override fun equals(other: Any?): Boolean =
        this === other || (other is Pose && matrix.contentEquals(other.matrix))

    override fun hashCode(): Int = matrix.contentHashCode()

    companion object {
        /** A 4x4 matrix. */
        const val MATRIX_ELEMENTS = 16

        fun identity(): Pose = Pose(
            floatArrayOf(
                1f, 0f, 0f, 0f,
                0f, 1f, 0f, 0f,
                0f, 0f, 1f, 0f,
                0f, 0f, 0f, 1f,
            ),
        )
    }
}

/** One captured frame's metadata. The pixels live on disk, referenced by [imageFileName]. */
@Serializable
data class CaptureFrame(
    val index: Int,
    val timestampNanos: Long,
    val imageFileName: String,
    val depthFileName: String? = null,
    val confidenceFileName: String? = null,
    val pose: Pose?,
    val intrinsics: CameraIntrinsics,
    val quality: FrameQuality,
    val isKeyframe: Boolean,
    /** Perceptual hash, used to reject near-duplicates. */
    val imageHash: Long = 0,
    val trackingState: TrackingState = TrackingState.Tracking,
)

@Serializable
enum class TrackingState { Tracking, Limited, Lost, Initialising }

/** How usable a frame is, as measured during capture. */
@Serializable
data class FrameQuality(
    val sharpness: Float = 0f,
    val exposure: Float = 0f,
    val contrast: Float = 0f,
    val clippedLowFraction: Float = 0f,
    val clippedHighFraction: Float = 0f,
    val acceptable: Boolean = false,
) {
    /** The single reason a frame was rejected, for the capture overlay to show. */
    val rejectionReason: FrameRejection?
        get() = when {
            acceptable -> null
            sharpness < MIN_SHARPNESS -> FrameRejection.Blurred
            exposure < MIN_EXPOSURE -> FrameRejection.TooDark
            exposure > MAX_EXPOSURE -> FrameRejection.TooBright
            clippedHighFraction > MAX_CLIPPED -> FrameRejection.Overexposed
            clippedLowFraction > MAX_CLIPPED -> FrameRejection.Underexposed
            contrast < MIN_CONTRAST -> FrameRejection.LowTexture
            else -> FrameRejection.Unknown
        }

    companion object {
        const val MIN_SHARPNESS = 90f
        const val MIN_EXPOSURE = 35f
        const val MAX_EXPOSURE = 225f
        const val MAX_CLIPPED = 0.22f
        const val MIN_CONTRAST = 16f
    }
}

@Serializable
enum class FrameRejection {
    Blurred,
    TooDark,
    TooBright,
    Overexposed,
    Underexposed,
    LowTexture,
    Duplicate,
    NoPose,
    Unknown,
}

/** A detected planar surface, as reported during capture. */
@Serializable
data class DetectedPlane(
    val id: String,
    val centerX: Float,
    val centerY: Float,
    val centerZ: Float,
    val normalX: Float,
    val normalY: Float,
    val normalZ: Float,
    val extentX: Float,
    val extentZ: Float,
    val kind: PlaneKind,
)

@Serializable
enum class PlaneKind { HorizontalUpward, HorizontalDownward, Vertical, Unknown }
