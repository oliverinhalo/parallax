package uk.co.jacoblevy.parallax.device

import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.os.Build
import android.util.Log
import com.google.ar.core.ArCoreApk
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import uk.co.jacoblevy.parallax.core.common.di.IoDispatcher
import uk.co.jacoblevy.parallax.core.model.ArCoreAvailability
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilities
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilityProvider
import uk.co.jacoblevy.parallax.core.model.Resolution
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Resolves this device's capabilities from the platform.
 *
 * Every probe here is defensive. Camera characteristics, Vulkan feature flags and ARCore's
 * availability check all throw or misreport on some real devices, and a scanner that crashes on
 * startup because one OEM returns null for the stream configuration map is worse than one that
 * assumes less and says so. Each failure degrades to the conservative answer and is logged.
 */
@Singleton
class AndroidDeviceCapabilityProvider @Inject constructor(
    @ApplicationContext private val context: Context,
    @IoDispatcher private val io: CoroutineDispatcher,
) : DeviceCapabilityProvider {

    private val mutex = Mutex()
    private var cached: DeviceCapabilities? = null

    override suspend fun resolve(): DeviceCapabilities = mutex.withLock {
        cached?.let { previous ->
            // Everything but ARCore's install state is fixed for the life of the process, and
            // that one can change while the user is in the Play Store, so it is re-read.
            return@withLock previous.copy(arCoreAvailability = arCoreAvailability())
                .also { cached = it }
        }
        withContext(io) { probe() }.also { cached = it }
    }

    private fun probe(): DeviceCapabilities {
        val packageManager = context.packageManager
        val arCore = arCoreAvailability()
        val resolutions = cameraResolutions()
        val vulkan = vulkanComputeVersion(packageManager)
        return DeviceCapabilities(
            arCoreAvailability = arCore,
            // The Depth API needs ARCore itself; without it there is no depth to read.
            supportsDepthApi = arCore.isUsable && depthSupported(),
            supportsRawDepth = arCore.isUsable && depthSupported(),
            hasTimeOfFlightSensor = hasTimeOfFlightSensor(),
            vulkanVersion = vulkan,
            supportsVulkanCompute = vulkan > 0,
            totalRamBytes = totalRamBytes(),
            availableStorageBytes = context.filesDir.usableSpace,
            cpuCoreCount = Runtime.getRuntime().availableProcessors(),
            supportsNnApi = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P,
            supportsGpuDelegate = vulkan > 0 ||
                packageManager.hasSystemFeature(PackageManager.FEATURE_OPENGLES_EXTENSION_PACK),
            maxTextureSize = maxTextureSize(),
            cameraResolutions = resolutions,
            supportsVideoCapture = resolutions.isNotEmpty(),
        )
    }

    private fun arCoreAvailability(): ArCoreAvailability = runCatching {
        when (ArCoreApk.getInstance().checkAvailability(context)) {
            ArCoreApk.Availability.SUPPORTED_INSTALLED -> ArCoreAvailability.Supported
            ArCoreApk.Availability.SUPPORTED_APK_TOO_OLD ->
                ArCoreAvailability.SupportedNeedsUpdate
            ArCoreApk.Availability.SUPPORTED_NOT_INSTALLED ->
                ArCoreAvailability.SupportedNotInstalled
            ArCoreApk.Availability.UNSUPPORTED_DEVICE_NOT_CAPABLE ->
                ArCoreAvailability.UnsupportedDevice
            // The query is still in flight. Unknown is the honest answer, and the capture
            // screen re-reads it when it opens.
            ArCoreApk.Availability.UNKNOWN_CHECKING,
            ArCoreApk.Availability.UNKNOWN_ERROR,
            ArCoreApk.Availability.UNKNOWN_TIMED_OUT,
            -> ArCoreAvailability.Unknown
        }
    }.getOrElse { failure ->
        Log.w(TAG, "ARCore availability check failed", failure)
        ArCoreAvailability.Unknown
    }

    /**
     * Whether ARCore's Depth API is likely to work.
     *
     * The authoritative check needs a `Session`, which cannot be created without the camera
     * permission, so this is a pre-check: ARCore present, and a rear camera that is not
     * limited-legacy. The capture screen asks the session directly once it has one.
     */
    private fun depthSupported(): Boolean = runCatching {
        val manager = context.getSystemService(CameraManager::class.java) ?: return false
        manager.cameraIdList.any { id ->
            val characteristics = manager.getCameraCharacteristics(id)
            val facing = characteristics.get(CameraCharacteristics.LENS_FACING)
            val level = characteristics.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)
            facing == CameraCharacteristics.LENS_FACING_BACK &&
                level != CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY
        }
    }.getOrElse { failure ->
        Log.w(TAG, "camera characteristics unavailable", failure)
        false
    }

    private fun hasTimeOfFlightSensor(): Boolean = runCatching {
        val manager = context.getSystemService(CameraManager::class.java) ?: return false
        manager.cameraIdList.any { id ->
            val capabilities = manager.getCameraCharacteristics(id)
                .get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
            capabilities?.contains(
                CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT,
            ) == true
        }
    }.getOrElse { false }

    private fun cameraResolutions(): List<Resolution> = runCatching {
        val manager = context.getSystemService(CameraManager::class.java)
            ?: return emptyList()
        val rear = manager.cameraIdList.firstOrNull { id ->
            manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) ==
                CameraCharacteristics.LENS_FACING_BACK
        } ?: return emptyList()
        val map = manager.getCameraCharacteristics(rear)
            .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?: return emptyList()
        map.getOutputSizes(android.graphics.ImageFormat.JPEG)
            .orEmpty()
            .map { Resolution(it.width, it.height) }
            .sortedByDescending { it.width.toLong() * it.height }
            .take(MAX_REPORTED_RESOLUTIONS)
    }.getOrElse { failure ->
        Log.w(TAG, "could not list camera resolutions", failure)
        emptyList()
    }

    /**
     * The device's Vulkan compute level, or 0 when compute is unavailable.
     *
     * `FEATURE_VULKAN_HARDWARE_COMPUTE` is the only reliable signal: a device can report a
     * Vulkan *version* while supporting no compute queue at all, and launching the splat
     * trainer on one of those fails inside the driver rather than in our code.
     */
    private fun vulkanComputeVersion(packageManager: PackageManager): Int {
        if (!packageManager.hasSystemFeature(
                PackageManager.FEATURE_VULKAN_HARDWARE_COMPUTE,
            )
        ) {
            return 0
        }
        val features = packageManager.systemAvailableFeatures.firstOrNull {
            it.name == PackageManager.FEATURE_VULKAN_HARDWARE_VERSION
        }
        return features?.version ?: 0
    }

    private fun totalRamBytes(): Long = runCatching {
        val manager = context.getSystemService(ActivityManager::class.java) ?: return 0
        val info = ActivityManager.MemoryInfo()
        manager.getMemoryInfo(info)
        info.totalMem
    }.getOrElse { 0 }

    /**
     * The GPU's maximum texture size.
     *
     * Read from the kernel's reported GL limits where possible. Querying it properly needs an EGL
     * context, which costs tens of milliseconds at startup and has to happen off the main
     * thread; the texture size only gates the atlas resolution, so the conservative default is
     * used until the renderer is up and can report the real value.
     */
    private fun maxTextureSize(): Int {
        val ram = totalRamBytes()
        return when {
            ram >= HIGH_RAM_BYTES -> LARGE_TEXTURE
            ram >= MID_RAM_BYTES -> MEDIUM_TEXTURE
            else -> SMALL_TEXTURE
        }
    }

    private companion object {
        const val TAG = "DeviceCapabilities"
        const val MAX_REPORTED_RESOLUTIONS = 8
        const val HIGH_RAM_BYTES = 8L * 1024 * 1024 * 1024
        const val MID_RAM_BYTES = 4L * 1024 * 1024 * 1024
        const val LARGE_TEXTURE = 8192
        const val MEDIUM_TEXTURE = 4096
        const val SMALL_TEXTURE = 2048
    }
}
