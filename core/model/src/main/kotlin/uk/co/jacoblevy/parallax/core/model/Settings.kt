package uk.co.jacoblevy.parallax.core.model

import kotlinx.serialization.Serializable

/** Everything the settings screen controls. Defaults are what a new install gets. */
@Serializable
data class AppSettings(
    val theme: ThemeMode = ThemeMode.Dark,
    /** Material You dynamic colour. Off by default: the brand palette is the default identity. */
    val useDynamicColor: Boolean = false,
    val units: LengthUnit = LengthUnit.Metres,
    val defaultQuality: QualityPreset = QualityPreset.Balanced,
    val defaultCaptureMode: CaptureMode = CaptureMode.Object,
    val haptics: Boolean = true,
    val guidanceVoicePrompts: Boolean = false,
    val keepRawFramesAfterProcessing: Boolean = false,
    val processOnlyWhileCharging: Boolean = false,
    val processOnlyOnUnmeteredNetwork: Boolean = false,
    val trainSplatsInBackground: Boolean = true,
    val showAdvancedStats: Boolean = false,
    val onboardingComplete: Boolean = false,
    val tutorialComplete: Boolean = false,
    /** Soft cap on total project storage. 0 means no limit. */
    val storageBudgetBytes: Long = 0,
) {
    val imperial: Boolean get() = units.isImperial
}

@Serializable
enum class ThemeMode(val label: String) {
    Dark("Dark"),
    Light("Light"),
    System("Follow system"),
}

/** Storage usage, broken down for the settings dashboard. */
@Serializable
data class StorageBreakdown(
    val rawFramesBytes: Long = 0,
    val depthBytes: Long = 0,
    val checkpointBytes: Long = 0,
    val meshBytes: Long = 0,
    val textureBytes: Long = 0,
    val pointCloudBytes: Long = 0,
    val splatBytes: Long = 0,
    val thumbnailBytes: Long = 0,
    val exportBytes: Long = 0,
    val deviceFreeBytes: Long = 0,
) {
    val totalBytes: Long
        get() = rawFramesBytes + depthBytes + checkpointBytes + meshBytes + textureBytes +
            pointCloudBytes + splatBytes + thumbnailBytes + exportBytes

    /** What can be deleted without losing a finished model. */
    val reclaimableBytes: Long get() = rawFramesBytes + depthBytes + checkpointBytes

    val categories: List<StorageCategory>
        get() = listOf(
            StorageCategory("Raw frames", rawFramesBytes, reclaimable = true),
            StorageCategory("Depth maps", depthBytes, reclaimable = true),
            StorageCategory("Checkpoints", checkpointBytes, reclaimable = true),
            StorageCategory("Meshes", meshBytes, reclaimable = false),
            StorageCategory("Textures", textureBytes, reclaimable = false),
            StorageCategory("Point clouds", pointCloudBytes, reclaimable = false),
            StorageCategory("Splats", splatBytes, reclaimable = false),
            StorageCategory("Thumbnails", thumbnailBytes, reclaimable = false),
            StorageCategory("Exports", exportBytes, reclaimable = true),
        ).filter { it.bytes > 0 }
}

@Serializable
data class StorageCategory(val label: String, val bytes: Long, val reclaimable: Boolean)

/** How the library is currently sorted and filtered. */
@Serializable
data class LibraryQuery(
    val search: String = "",
    val sort: LibrarySort = LibrarySort.NewestFirst,
    val folderId: String? = null,
    val modeFilter: CaptureMode? = null,
    val favouritesOnly: Boolean = false,
) {
    val isFiltered: Boolean
        get() = search.isNotBlank() || modeFilter != null || favouritesOnly || folderId != null
}

@Serializable
enum class LibrarySort(val label: String) {
    NewestFirst("Newest first"),
    OldestFirst("Oldest first"),
    NameAscending("Name A–Z"),
    NameDescending("Name Z–A"),
    LargestFirst("Largest first"),
    SmallestFirst("Smallest first"),
}
