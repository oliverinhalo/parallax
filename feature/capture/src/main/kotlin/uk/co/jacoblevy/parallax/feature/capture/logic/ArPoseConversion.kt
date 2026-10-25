package uk.co.jacoblevy.parallax.feature.capture.logic

import uk.co.jacoblevy.parallax.core.model.Pose

/**
 * ARCore's camera axes are not the engine's.
 *
 * ARCore gives a camera pose in the GL convention: +X right, **+Y up, −Z forward**. The
 * reconstruction engine uses the OpenCV image convention: +X right, **+Y down, +Z forward**,
 * which is what its pinhole projection assumes and what the synthetic captures in its tests are
 * built in. The two differ by a half turn about X.
 *
 * Without the turn every world point lands behind the camera, nothing projects into any image,
 * and fusion integrates twenty-eight frames of perfectly good depth into an empty volume. The
 * failure says the depth maps and the camera poses disagree, which is true and says nothing
 * about which of the two is wrong. It cost three failed scans to find, and the code that was
 * wrong was a comment asserting the conventions already matched.
 *
 * Done here rather than inside the session wrapper so it is ordinary testable code: the session
 * is one of the files that cannot run in CI, and this is a decision rather than a device call.
 */
object ArPoseConversion {

    /**
     * Turn an ARCore world-from-camera matrix into the engine's.
     *
     * Both are column-major 4x4. The half turn about X is a right-multiplication by
     * `diag(1, -1, -1, 1)` -- it acts on the camera's axes, not the world's -- which in
     * column-major layout negates the second and third columns and leaves the camera's position
     * in the fourth alone.
     */
    fun toEngineConvention(arCoreWorldFromCamera: FloatArray): FloatArray {
        require(arCoreWorldFromCamera.size == Pose.MATRIX_ELEMENTS) {
            "a pose matrix must have ${Pose.MATRIX_ELEMENTS} elements, " +
                "got ${arCoreWorldFromCamera.size}"
        }
        val out = arCoreWorldFromCamera.copyOf()
        for (index in Y_COLUMN until W_COLUMN) out[index] = -out[index]
        return out
    }

    /** Column-major index of the first element of the camera's Y axis column. */
    private const val Y_COLUMN = 4

    /** And of the position column, which is left as it is. */
    private const val W_COLUMN = 12
}
