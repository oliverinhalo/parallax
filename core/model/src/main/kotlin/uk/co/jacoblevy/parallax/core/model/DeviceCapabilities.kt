package uk.co.jacoblevy.parallax.core.model

import kotlinx.serialization.Serializable

/**
 * What this device can actually do, resolved once at startup.
 *
 * The app adapts rather than refusing: a phone without ARCore can still build models from photos
 * via full structure-from-motion, and one without Vulkan still gets meshes and point clouds. Each
 * limitation carries the user-facing explanation, so the UI never silently hides a feature.
 */
@Serializable
data class DeviceCapabilities(
    val arCoreAvailability: ArCoreAvailability = ArCoreAvailability.Unknown,
    val supportsDepthApi: Boolean = false,
    val supportsRawDepth: Boolean = false,
    val hasTimeOfFlightSensor: Boolean = false,
    val vulkanVersion: Int = 0,
    val supportsVulkanCompute: Boolean = false,
    val totalRamBytes: Long = 0,
    val availableStorageBytes: Long = 0,
    val cpuCoreCount: Int = 1,
    val supportsNnApi: Boolean = false,
    val supportsGpuDelegate: Boolean = false,
    val maxTextureSize: Int = 4096,
    val cameraResolutions: List<Resolution> = emptyList(),
    val supportsVideoCapture: Boolean = true,
) {
    /** The highest capture mode set this device can offer. */
    val availableCaptureModes: List<CaptureMode>
        get() = buildList {
            if (arCoreAvailability.isUsable) {
                add(CaptureMode.Object)
                add(CaptureMode.Room)
                add(CaptureMode.Area)
            }
            // Import always works: it needs no AR support at all.
            add(CaptureMode.Import)
        }

    /** Highest quality preset this device has the memory to run. */
    val maxQualityPreset: QualityPreset
        get() = when {
            totalRamBytes >= 10L * 1024 * 1024 * 1024 -> QualityPreset.Max
            totalRamBytes >= 6L * 1024 * 1024 * 1024 -> QualityPreset.Balanced
            else -> QualityPreset.Fast
        }

    /**
     * Memory budget for the reconstruction pipeline.
     *
     * A quarter of physical RAM, capped at 2 GB. Android will kill a background process long
     * before it can use everything, and the TSDF volume is the one allocation big enough to
     * matter, so it is sized from this rather than from whatever happens to be free.
     */
    val reconstructionMemoryBudgetBytes: Long
        get() = (totalRamBytes / 4).coerceAtMost(2L * 1024 * 1024 * 1024).coerceAtLeast(
            256L * 1024 * 1024,
        )

    /**
     * Memory the splat optimiser may use.
     *
     * Deliberately much tighter than [reconstructionMemoryBudgetBytes]: that
     * one sizes a TSDF volume that is allocated, filled and freed, whereas
     * this working set is held for the whole of training while the frames,
     * the point cloud and the renderer's own buffers are also live. A
     * twenty-fourth of physical RAM, capped at 384 MB.
     */
    val splatMemoryBudgetBytes: Long
        get() = (totalRamBytes / 24).coerceAtMost(384L * 1024 * 1024)

    /**
     * Whether a splat field can be trained at all.
     *
     * Memory is the hard requirement: the optimiser holds the model, its
     * gradients and two Adam moments per parameter at once, which is four
     * copies of a cloud that is already the largest thing the app builds.
     */
    val canTrainSplats: Boolean
        get() = totalRamBytes >= 4L * 1024 * 1024 * 1024

    /**
     * Whether splat training will run on the GPU.
     *
     * Without Vulkan compute it falls back to the CPU reference, which is
     * correct but far slower, so the schedule is cut to a fifth of the
     * iterations and a third of the gaussians. The result is a usable splat
     * field rather than none, and the settings screen says so rather than
     * letting the difference come as a surprise.
     */
    val splatsTrainOnGpu: Boolean
        get() = canTrainSplats && supportsVulkanCompute && totalRamBytes >= 6L * 1024 * 1024 * 1024

    /** Every reason some feature is unavailable, for the settings screen to list. */
    val limitations: List<DeviceLimitation>
        get() = buildList {
            if (!arCoreAvailability.isUsable) {
                add(
                    DeviceLimitation(
                        title = "AR scanning unavailable",
                        detail = arCoreAvailability.explanation,
                        affects = "Object, Room and Area modes. You can still build models " +
                            "from photos and video.",
                    ),
                )
            }
            if (!supportsDepthApi && arCoreAvailability.isUsable) {
                add(
                    DeviceLimitation(
                        title = "No depth sensing",
                        detail = "This device cannot provide per-pixel depth.",
                        affects = "Depth is estimated from the images instead, which is slower " +
                            "and slightly less accurate on plain surfaces.",
                    ),
                )
            }
            if (!canTrainSplats) {
                add(
                    DeviceLimitation(
                        title = "Gaussian splatting unavailable",
                        detail = "At least 4 GB of RAM is needed to train splats.",
                        affects = "Meshes and point clouds work normally.",
                    ),
                )
            } else if (!splatsTrainOnGpu) {
                add(
                    DeviceLimitation(
                        title = "Gaussian splatting runs on the CPU",
                        detail = if (!supportsVulkanCompute) {
                            "Vulkan compute is not supported on this device."
                        } else {
                            "At least 6 GB of RAM is needed for GPU splat training."
                        },
                        affects = "Splat scans still work, but take considerably longer and " +
                            "come out with less detail.",
                    ),
                )
            }
            if (maxQualityPreset != QualityPreset.Max) {
                add(
                    DeviceLimitation(
                        title = "Max quality unavailable",
                        detail = "Max quality needs at least 10 GB of RAM.",
                        affects = "Up to ${maxQualityPreset.label} quality on this device.",
                    ),
                )
            }
        }
}

@Serializable
data class DeviceLimitation(val title: String, val detail: String, val affects: String)

@Serializable
enum class ArCoreAvailability(val explanation: String) {
    Supported("ARCore is installed and ready."),
    SupportedNeedsUpdate("ARCore needs to be updated before AR scanning will work."),
    SupportedNotInstalled("ARCore is not installed. Install it to enable AR scanning."),
    UnsupportedDevice("This device is not on Google's ARCore supported list."),
    Unknown("ARCore support has not been determined yet."),
    ;

    val isUsable: Boolean get() = this == Supported
    val canBeFixedByUser: Boolean
        get() = this == SupportedNeedsUpdate || this == SupportedNotInstalled
}

@Serializable
data class Resolution(val width: Int, val height: Int) {
    val megapixels: Float get() = width * height / 1_000_000f
    override fun toString(): String = "${width}x$height"
}

/**
 * Resolves what this device can actually do.
 *
 * A port rather than a concrete class: answering the question needs ARCore, Vulkan, the camera
 * list and `ActivityManager`, and neither the settings screen nor the capture screen should have
 * to carry those dependencies just to ask whether splat training is possible. The
 * implementation lives in `:app`.
 *
 * Implementations are expected to cache. Nothing here can change while the process is alive
 * except ARCore's install state, which is re-read when the capture screen opens.
 */
interface DeviceCapabilityProvider {
    suspend fun resolve(): DeviceCapabilities
}
