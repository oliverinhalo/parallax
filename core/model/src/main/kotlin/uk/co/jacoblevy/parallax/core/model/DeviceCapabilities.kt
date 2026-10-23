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

    val canTrainSplats: Boolean
        get() = supportsVulkanCompute && totalRamBytes >= 6L * 1024 * 1024 * 1024

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
                        detail = if (!supportsVulkanCompute) {
                            "Vulkan compute is not supported."
                        } else {
                            "At least 6 GB of RAM is needed to train splats."
                        },
                        affects = "Meshes and point clouds work normally.",
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
