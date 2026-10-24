package uk.co.jacoblevy.parallax.engine.ml

import android.content.Context
import android.content.res.AssetManager
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The optional model files, and why they are optional.
 *
 * Parallax ships no model weights in the repository. Two reasons, and both are deliberate:
 *
 * 1. **Licensing has to be checked per file.** Depth Anything V2 is Apache-2.0 only in its
 *    **Small** variant; the Base and Large weights are non-commercial. A repository that
 *    vendors weights invites someone to swap in a bigger one, and nothing would catch it.
 * 2. **Nothing should depend on them.** A scanner that cannot reconstruct without a 50 MB
 *    download is not an on-device scanner. Multi-view stereo in the native engine covers every
 *    device; these models make it better where they are present, not possible.
 *
 * To enable them, drop the files into `engine/ml/src/main/assets/models/` with the names below.
 * `docs/LICENSES.md` records the licence each one must carry, and the build does not fetch
 * anything: an on-device app with no network permission cannot, and should not, download its
 * own weights at runtime.
 */
enum class MlModel(val assetPath: String, val description: String) {
    /**
     * Monocular relative depth. Apache-2.0 in the Small variant only.
     *
     * Used to fill in depth on surfaces multi-view stereo cannot measure -- a blank wall, a
     * glossy surface -- after being fitted to metric scale against the sparse points.
     */
    MonocularDepth(
        assetPath = "models/depth_anything_v2_small.tflite",
        description = "Monocular depth (Depth Anything V2 Small, Apache-2.0)",
    ),

    /**
     * Subject segmentation, for object mode's background removal.
     *
     * Without it, object mode falls back to the geometric cutout: remove the supporting plane,
     * keep the largest connected component. That works well for an object on a table and less
     * well for one against a cluttered background.
     */
    SubjectSegmentation(
        assetPath = "models/subject_segmentation.tflite",
        description = "Subject segmentation (Apache-2.0 or MIT weights only)",
    ),
}

/** Which optional models this build actually has. */
@Singleton
class MlModelRegistry @Inject constructor(
    @ApplicationContext private val context: Context,
) {
    private val present: Set<MlModel> by lazy {
        MlModel.entries.filterTo(mutableSetOf()) { model -> exists(context.assets, model) }
    }

    fun isAvailable(model: MlModel): Boolean = model in present

    val availableModels: Set<MlModel> get() = present

    /**
     * Why a model is unavailable, in words a settings screen can show.
     *
     * Named rather than silent: a user wondering why their scan of a white wall came out poorly
     * deserves to know that the optional depth model is not installed.
     */
    fun unavailableReason(model: MlModel): String? =
        if (isAvailable(model)) {
            null
        } else {
            "${model.description} is not installed in this build. " +
                "Scanning still works; depth is measured from the images instead."
        }

    private fun exists(assets: AssetManager, model: MlModel): Boolean =
        runCatching { assets.open(model.assetPath).use { true } }.getOrDefault(false)
}
