package uk.co.jacoblevy.parallax.core.data

import uk.co.jacoblevy.parallax.core.data.storage.CaptureManifest
import uk.co.jacoblevy.parallax.core.model.CameraIntrinsics
import uk.co.jacoblevy.parallax.core.model.CaptureFrame
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.FrameQuality
import uk.co.jacoblevy.parallax.core.model.Pose

internal object TestFixtures {

    val intrinsics = CameraIntrinsics(
        focalLengthX = 1_440f,
        focalLengthY = 1_440f,
        principalPointX = 960f,
        principalPointY = 540f,
        imageWidth = 1_920,
        imageHeight = 1_080,
    )

    fun frames(count: Int, withPoses: Boolean = true): List<CaptureFrame> = (0 until count).map { i ->
        CaptureFrame(
            index = i,
            timestampNanos = i * 33_000_000L,
            imageFileName = "%06d.jpg".format(i),
            depthFileName = "%06d.bin".format(i),
            pose = if (withPoses) Pose.identity() else null,
            intrinsics = intrinsics,
            quality = FrameQuality(
                sharpness = 140f,
                exposure = 120f,
                contrast = 40f,
                acceptable = true,
            ),
            isKeyframe = i % 2 == 0,
        )
    }

    fun manifest(projectId: String, withPoses: Boolean = true): CaptureManifest =
        CaptureManifest.from(
            projectId = projectId,
            mode = CaptureMode.Object,
            createdAtMillis = 1_700_000_000_000,
            frames = frames(count = 4, withPoses = withPoses),
        )
}
