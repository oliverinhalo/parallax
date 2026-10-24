package uk.co.jacoblevy.parallax.core.model

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import kotlin.math.abs

class DomainModelTest {

    // ---- reconstruction stages --------------------------------------------

    @Test
    fun `stage weights sum to one so the progress bar reaches the end`() {
        val total = ReconstructionStage.entries.sumOf { it.weight.toDouble() }
        assertThat(abs(total - 1.0)).isLessThan(1e-6)
    }

    @Test
    fun `overall progress is monotonic across the whole pipeline`() {
        var previous = -1f
        for (stage in ReconstructionStage.entries) {
            for (fraction in listOf(0f, 0.5f, 1f)) {
                val overall = ReconstructionStage.overallProgress(stage, fraction)
                // The tolerance is for float summation across ten weights, not for a real
                // regression: a stage boundary may land an ulp below the previous stage's end.
                assertThat(overall).isAtLeast(previous - 1e-6f)
                previous = overall
            }
        }
        assertThat(previous).isWithin(1e-5f).of(1f)
    }

    @Test
    fun `overall progress clamps a stage progress that is out of range`() {
        val stage = ReconstructionStage.Fusion
        assertThat(ReconstructionStage.overallProgress(stage, -1f))
            .isEqualTo(ReconstructionStage.overallProgress(stage, 0f))
        assertThat(ReconstructionStage.overallProgress(stage, 2f))
            .isEqualTo(ReconstructionStage.overallProgress(stage, 1f))
    }

    @Test
    fun `the last stage has no successor`() {
        val last = ReconstructionStage.entries.last()
        assertThat(ReconstructionStage.next(last)).isNull()
        assertThat(ReconstructionStage.next(ReconstructionStage.Ingest))
            .isEqualTo(ReconstructionStage.PoseRefinement)
    }

    // ---- quality presets ---------------------------------------------------

    @Test
    fun `presets are ordered by cost in every dimension`() {
        val presets = listOf(QualityPreset.Fast, QualityPreset.Balanced, QualityPreset.Max)
        for ((lower, higher) in presets.zipWithNext()) {
            assertThat(higher.voxelSizeMetres).isLessThan(lower.voxelSizeMetres)
            assertThat(higher.maxKeyframes).isGreaterThan(lower.maxKeyframes)
            assertThat(higher.textureResolution).isAtLeast(lower.textureResolution)
            assertThat(higher.targetTriangles).isGreaterThan(lower.targetTriangles)
            assertThat(higher.secondsPerKeyframe).isGreaterThan(lower.secondsPerKeyframe)
        }
    }

    @Test
    fun `the time estimate is capped by the preset's own keyframe limit`() {
        // Otherwise a 2,000-frame capture would be quoted an hour on Fast, which caps at 80.
        val capped = QualityPreset.Fast.estimatedMillis(keyframes = 10_000)
        val atLimit = QualityPreset.Fast.estimatedMillis(keyframes = QualityPreset.Fast.maxKeyframes)
        assertThat(capped).isEqualTo(atLimit)
    }

    @Test
    fun `the storage estimate grows with quality and with scene size`() {
        val small = QualityPreset.Balanced.estimatedBytes(keyframes = 50, sceneExtentMetres = 1f)
        val large = QualityPreset.Balanced.estimatedBytes(keyframes = 50, sceneExtentMetres = 6f)
        val max = QualityPreset.Max.estimatedBytes(keyframes = 50, sceneExtentMetres = 1f)
        assertThat(large).isGreaterThan(small)
        assertThat(max).isGreaterThan(small)
    }

    @Test
    fun `only the slower presets train splats`() {
        assertThat(QualityPreset.Fast.runSplatTraining).isFalse()
        assertThat(QualityPreset.Max.runSplatTraining).isTrue()
    }

    // ---- project state ----------------------------------------------------

    private fun project(status: ProjectStatus, outputs: Set<OutputKind> = emptySet()) = Project(
        id = "p",
        name = "n",
        captureMode = CaptureMode.Object,
        createdAtMillis = 0,
        updatedAtMillis = 0,
        status = status,
        outputs = outputs,
    )

    @Test
    fun `a project can only be processed from a captured or failed state`() {
        assertThat(project(ProjectStatus.Captured).canProcess).isTrue()
        assertThat(
            project(ProjectStatus.Failed(ReconstructionStage.Fusion, "x", null)).canProcess,
        ).isTrue()
        assertThat(project(ProjectStatus.Capturing(4)).canProcess).isFalse()
        assertThat(project(ProjectStatus.Queued(1)).canProcess).isFalse()
        assertThat(project(ProjectStatus.Ready()).canProcess).isFalse()
    }

    @Test
    fun `a floor plan alone is not something the viewer can show`() {
        assertThat(project(ProjectStatus.Ready(), setOf(OutputKind.FloorPlan)).hasViewableOutput)
            .isFalse()
        assertThat(project(ProjectStatus.Ready(), setOf(OutputKind.PointCloud)).hasViewableOutput)
            .isTrue()
    }

    @Test
    fun `import mode is the one mode that does not need ARCore`() {
        assertThat(CaptureMode.Import.requiresArCore).isFalse()
        for (mode in CaptureMode.entries - CaptureMode.Import) {
            assertThat(mode.requiresArCore).isTrue()
        }
    }

    // ---- capture ---------------------------------------------------------

    @Test
    fun `intrinsics rescale for a different image size`() {
        val colour = CameraIntrinsics(1_440f, 1_440f, 960f, 540f, 1_920, 1_080)
        val depth = colour.scaledTo(320, 180)
        assertThat(depth.focalLengthX).isWithin(1e-3f).of(240f)
        assertThat(depth.principalPointX).isWithin(1e-3f).of(160f)
        assertThat(depth.imageWidth).isEqualTo(320)
        // The field of view is a property of the lens and must not change with the resolution.
        assertThat(depth.horizontalFovDegrees).isWithin(1e-2f).of(colour.horizontalFovDegrees)
    }

    @Test
    fun `invalid intrinsics are reported rather than producing a nonsense field of view`() {
        val broken = CameraIntrinsics(0f, 0f, 0f, 0f, 0, 0)
        assertThat(broken.isValid).isFalse()
        assertThat(broken.horizontalFovDegrees).isEqualTo(0f)
        // Rescaling an invalid set is a no-op rather than a divide by zero.
        assertThat(broken.scaledTo(100, 100)).isEqualTo(broken)
    }

    @Test
    fun `a pose must be a four by four matrix`() {
        val failure = runCatching { Pose(FloatArray(9)) }.exceptionOrNull()
        assertThat(failure).isInstanceOf(IllegalArgumentException::class.java)
    }

    @Test
    fun `pose distance is between camera centres`() {
        val origin = Pose.identity()
        val moved = Pose(origin.matrix.copyOf().also { it[12] = 3f; it[13] = 4f })
        assertThat(origin.distanceTo(moved)).isWithin(1e-4f).of(5f)
    }

    @Test
    fun `an acceptable frame has no rejection reason and a bad one names the worst problem`() {
        assertThat(FrameQuality(acceptable = true).rejectionReason).isNull()

        val blurred = FrameQuality(sharpness = 10f, exposure = 120f, contrast = 40f)
        assertThat(blurred.rejectionReason).isEqualTo(FrameRejection.Blurred)

        val dark = FrameQuality(sharpness = 200f, exposure = 5f, contrast = 40f)
        assertThat(dark.rejectionReason).isEqualTo(FrameRejection.TooDark)

        val flat = FrameQuality(sharpness = 200f, exposure = 120f, contrast = 2f)
        assertThat(flat.rejectionReason).isEqualTo(FrameRejection.LowTexture)
    }

    // ---- guidance --------------------------------------------------------

    @Test
    fun `the prompts that save a scan outrank the ones that merely improve it`() {
        val urgent = listOf(
            GuidancePrompt.LowStorage,
            GuidancePrompt.HoldStill,
            GuidancePrompt.Overheating,
        )
        val advisory = listOf(
            GuidancePrompt.KeepGoing,
            GuidancePrompt.ReadyToFinish,
            GuidancePrompt.LowTexture,
        )
        val lowestUrgent = urgent.minOf { it.priority }
        val highestAdvisory = advisory.maxOf { it.priority }
        assertThat(lowestUrgent).isGreaterThan(highestAdvisory)
    }

    @Test
    fun `thermal throttling scales the capture rate down and stops only at emergency`() {
        assertThat(ThermalStatus.Nominal.captureRateScale).isEqualTo(1f)
        assertThat(ThermalStatus.Serious.captureRateScale).isLessThan(1f)
        assertThat(ThermalStatus.Critical.captureRateScale)
            .isLessThan(ThermalStatus.Serious.captureRateScale)
        assertThat(ThermalStatus.Emergency.mustStop).isTrue()
        for (status in ThermalStatus.entries - ThermalStatus.Emergency) {
            assertThat(status.mustStop).isFalse()
        }
    }

    @Test
    fun `coverage counts a bin once a quarter of it has been seen`() {
        val bins = FloatArray(CoverageMap.DEFAULT_AZIMUTH_BINS * CoverageMap.DEFAULT_ELEVATION_BINS)
        val map = CoverageMap(bins = bins)
        assertThat(map.coverage).isEqualTo(0f)

        bins.fill(1f, 0, bins.size / 2)
        assertThat(CoverageMap(bins = bins).coverage).isWithin(1e-5f).of(0.5f)

        // Just below the threshold does not count, which is what stops a glancing pass over the
        // back of a subject from reading as covered.
        val barely = FloatArray(bins.size) { 0.2f }
        assertThat(CoverageMap(bins = barely).coverage).isEqualTo(0f)
    }

    @Test
    fun `an out-of-range bin lookup returns zero rather than throwing`() {
        val map = CoverageMap()
        assertThat(map.binAt(-1, 0)).isEqualTo(0f)
        assertThat(map.binAt(0, map.azimuthBins)).isEqualTo(0f)
    }

    @Test
    fun `the rejection rate of an empty capture is zero, not a division by zero`() {
        assertThat(CaptureStats().rejectionRate).isEqualTo(0f)
        assertThat(CaptureStats(frameCount = 4, rejectedCount = 1).rejectionRate).isEqualTo(0.25f)
    }

    // ---- device capabilities ---------------------------------------------

    @Test
    fun `import is offered even on a device with no ARCore at all`() {
        val capabilities = DeviceCapabilities(
            arCoreAvailability = ArCoreAvailability.UnsupportedDevice,
        )
        assertThat(capabilities.availableCaptureModes).containsExactly(CaptureMode.Import)
    }

    @Test
    fun `a supported device offers every mode`() {
        val capabilities = DeviceCapabilities(arCoreAvailability = ArCoreAvailability.Supported)
        assertThat(capabilities.availableCaptureModes).containsExactlyElementsIn(CaptureMode.entries)
    }

    @Test
    fun `an ARCore state the user can fix is distinguished from one they cannot`() {
        assertThat(ArCoreAvailability.SupportedNotInstalled.canBeFixedByUser).isTrue()
        assertThat(ArCoreAvailability.SupportedNeedsUpdate.canBeFixedByUser).isTrue()
        assertThat(ArCoreAvailability.UnsupportedDevice.canBeFixedByUser).isFalse()
    }

    @Test
    fun `the memory budget is bounded at both ends`() {
        val tiny = DeviceCapabilities(totalRamBytes = 512L * 1024 * 1024)
        val huge = DeviceCapabilities(totalRamBytes = 64L * 1024 * 1024 * 1024)
        assertThat(tiny.reconstructionMemoryBudgetBytes).isAtLeast(256L * 1024 * 1024)
        assertThat(huge.reconstructionMemoryBudgetBytes).isEqualTo(2L * 1024 * 1024 * 1024)
    }

    @Test
    fun `splat training needs memory, and the GPU path needs Vulkan as well`() {
        val gb = 1024L * 1024 * 1024
        val capable = DeviceCapabilities(supportsVulkanCompute = true, totalRamBytes = 8 * gb)
        assertThat(capable.canTrainSplats).isTrue()
        assertThat(capable.splatsTrainOnGpu).isTrue()

        // No Vulkan compute still trains, on the CPU and on a shorter schedule.
        val noVulkan = DeviceCapabilities(supportsVulkanCompute = false, totalRamBytes = 8 * gb)
        assertThat(noVulkan.canTrainSplats).isTrue()
        assertThat(noVulkan.splatsTrainOnGpu).isFalse()

        // Six gigabytes is what the GPU path needs; four is enough for the CPU one.
        val middling = DeviceCapabilities(supportsVulkanCompute = true, totalRamBytes = 5 * gb)
        assertThat(middling.canTrainSplats).isTrue()
        assertThat(middling.splatsTrainOnGpu).isFalse()

        // Below four gigabytes the optimiser's four copies of the cloud do not fit.
        val small = DeviceCapabilities(supportsVulkanCompute = true, totalRamBytes = 3 * gb)
        assertThat(small.canTrainSplats).isFalse()
        assertThat(small.splatsTrainOnGpu).isFalse()
    }

    @Test
    fun `a preset that trains splats says how long for and how large`() {
        for (preset in QualityPreset.entries) {
            if (!preset.runSplatTraining) continue
            assertThat(preset.splatIterations).isGreaterThan(0)
            assertThat(preset.splatMaxGaussians).isGreaterThan(0)
        }
    }

    @Test
    fun `every limitation explains itself and says what it affects`() {
        // A feature that is simply missing from the UI reads as a bug, so each limitation has to
        // be presentable.
        val limited = DeviceCapabilities(
            arCoreAvailability = ArCoreAvailability.UnsupportedDevice,
            supportsVulkanCompute = false,
            totalRamBytes = 2L * 1024 * 1024 * 1024,
        )
        assertThat(limited.limitations).isNotEmpty()
        for (limitation in limited.limitations) {
            assertThat(limitation.title).isNotEmpty()
            assertThat(limitation.detail).isNotEmpty()
            assertThat(limitation.affects).isNotEmpty()
        }
    }

    @Test
    fun `a fully capable device reports no limitations`() {
        val capable = DeviceCapabilities(
            arCoreAvailability = ArCoreAvailability.Supported,
            supportsDepthApi = true,
            supportsVulkanCompute = true,
            totalRamBytes = 12L * 1024 * 1024 * 1024,
        )
        assertThat(capable.limitations).isEmpty()
        assertThat(capable.maxQualityPreset).isEqualTo(QualityPreset.Max)
    }

    // ---- export ----------------------------------------------------------

    @Test
    fun `a format is offered only when the project has what it needs`() {
        val meshOnly = setOf(OutputKind.Mesh)
        assertThat(ExportFormat.Glb.isAvailableFor(meshOnly)).isTrue()
        assertThat(ExportFormat.Ply.isAvailableFor(meshOnly)).isFalse()
        assertThat(ExportFormat.Spz.isAvailableFor(meshOnly)).isFalse()
        assertThat(ExportFormat.FloorPlanPdf.isAvailableFor(meshOnly)).isFalse()
        // STL needs a watertight mesh, which is produced alongside the ordinary one.
        assertThat(ExportFormat.Stl.isAvailableFor(meshOnly)).isTrue()
    }

    @Test
    fun `the splat and floor plan groupings match the formats in them`() {
        assertThat(ExportFormat.entries.filter { it.isSplat })
            .containsExactly(ExportFormat.SplatPly, ExportFormat.Splat, ExportFormat.Spz)
        assertThat(ExportFormat.entries.filter { it.isFloorPlan }).hasSize(3)
    }

    @Test
    fun `every format has a distinct label, a mime type and an explanation`() {
        assertThat(ExportFormat.entries.map { it.label }).containsNoDuplicates()
        for (format in ExportFormat.entries) {
            assertThat(format.extension).isNotEmpty()
            assertThat(format.mimeType).contains("/")
            assertThat(format.description).isNotEmpty()
        }
    }

    @Test
    fun `decimation never reduces a mesh below a closed shape`() {
        val options = ExportOptions(format = ExportFormat.Glb, decimationPercent = 1)
        assertThat(options.targetTriangles(sourceTriangles = 10)).isAtLeast(4)
        assertThat(ExportOptions(ExportFormat.Glb).targetTriangles(1_000)).isEqualTo(1_000)
    }

    @Test
    fun `unit conversions are exact where they should be`() {
        assertThat(LengthUnit.Feet.metresPerUnit).isEqualTo(0.3048)
        assertThat(LengthUnit.Inches.isImperial).isTrue()
        assertThat(LengthUnit.Millimetres.isImperial).isFalse()
    }

    // ---- measurements and bounds -----------------------------------------

    @Test
    fun `a distance measurement computes euclidean length`() {
        val distance = Measurement.Distance("m", "l", 0f, 0f, 0f, 3f, 4f, 0f)
        assertThat(distance.metres).isWithin(1e-6).of(5.0)
    }

    @Test
    fun `a height measurement ignores sign and horizontal separation`() {
        assertThat(Measurement.Height("m", "l", topY = 0f, baseY = 2.4f).metres)
            .isWithin(1e-6).of(2.4)
    }

    @Test
    fun `a rescale edit with a zero measurement is a no-op rather than infinite`() {
        assertThat(ModelEdit.Rescale(measuredMetres = 0.0, actualMetres = 2.0).factor)
            .isEqualTo(1.0)
        assertThat(ModelEdit.Rescale(measuredMetres = 1.0, actualMetres = 2.0).factor)
            .isEqualTo(2.0)
    }

    @Test
    fun `an unset bounding box reports itself as empty`() {
        assertThat(BoundingBox().isEmpty).isFalse()
        assertThat(BoundingBox(minX = 1f, maxX = 0f).isEmpty).isTrue()
        val box = BoundingBox(maxX = 3f, maxY = 4f, maxZ = 0f)
        assertThat(box.diagonal).isWithin(1e-4f).of(5f)
    }

    // ---- render modes and settings ---------------------------------------

    @Test
    fun `render modes are offered only for the outputs that exist`() {
        val textured = setOf(OutputKind.Mesh, OutputKind.TexturedMesh)
        assertThat(RenderMode.Textured.isAvailableFor(textured)).isTrue()
        assertThat(RenderMode.Textured.isAvailableFor(setOf(OutputKind.Mesh))).isFalse()
        assertThat(RenderMode.Shaded.isAvailableFor(setOf(OutputKind.Mesh))).isTrue()
        assertThat(RenderMode.Splat.isAvailableFor(textured)).isFalse()
    }

    @Test
    fun `the storage breakdown separates what can be reclaimed from what cannot`() {
        val breakdown = StorageBreakdown(
            rawFramesBytes = 100,
            depthBytes = 10,
            checkpointBytes = 1,
            meshBytes = 1_000,
            textureBytes = 500,
        )
        assertThat(breakdown.totalBytes).isEqualTo(1_611)
        assertThat(breakdown.reclaimableBytes).isEqualTo(111)
        // Empty categories are hidden rather than listed as zero.
        assertThat(breakdown.categories.map { it.label })
            .containsExactly("Raw frames", "Depth maps", "Checkpoints", "Meshes", "Textures")
    }

    @Test
    fun `a library query knows whether it is filtering anything`() {
        assertThat(LibraryQuery().isFiltered).isFalse()
        assertThat(LibraryQuery(search = "  ").isFiltered).isFalse()
        assertThat(LibraryQuery(favouritesOnly = true).isFiltered).isTrue()
        assertThat(LibraryQuery(folderId = "f").isFiltered).isTrue()
    }

    @Test
    fun `the default settings are the ones a new install should get`() {
        val defaults = AppSettings()
        assertThat(defaults.theme).isEqualTo(ThemeMode.Dark)
        assertThat(defaults.useDynamicColor).isFalse()
        assertThat(defaults.onboardingComplete).isFalse()
        assertThat(defaults.storageBudgetBytes).isEqualTo(0)
        assertThat(defaults.imperial).isFalse()
    }
}
