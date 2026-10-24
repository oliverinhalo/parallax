package uk.co.jacoblevy.parallax.core.data

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.data.database.toDomain
import uk.co.jacoblevy.parallax.core.data.database.toEntity
import uk.co.jacoblevy.parallax.core.model.BoundingBox
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.CaptureSummary
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.ModelMeasurements
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.Point3
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import uk.co.jacoblevy.parallax.core.model.ReconstructionStage

class MappersTest {

    private fun project(status: ProjectStatus) = Project(
        id = "p1",
        name = "Hallway",
        captureMode = CaptureMode.Room,
        createdAtMillis = 1_000,
        updatedAtMillis = 2_000,
        status = status,
        folderId = "f1",
        quality = QualityPreset.Max,
        capture = CaptureSummary(frameCount = 212, keyframeCount = 140, coverage = 0.82f),
        outputs = setOf(OutputKind.Mesh, OutputKind.TexturedMesh, OutputKind.FloorPlan),
        measurements = ModelMeasurements(
            triangleCount = 412_311,
            surfaceAreaSquareMetres = 55.25,
            boundsMetres = BoundingBox(maxX = 4.2f, maxY = 2.4f, maxZ = 3.1f),
            floorAreaSquareMetres = 13.02,
        ),
        storageBytes = 91_000_000,
        favourite = true,
        notes = "the one with the awkward corner",
    )

    @Test
    fun `project survives a round trip through the entity`() {
        val original = project(ProjectStatus.Ready(refiningSplats = true))
        assertThat(original.toEntity().toDomain()).isEqualTo(original)
    }

    @Test
    fun `every status variant round trips`() {
        val statuses = listOf(
            ProjectStatus.Capturing(frameCount = 17),
            ProjectStatus.Captured,
            ProjectStatus.Queued(position = 3),
            ProjectStatus.Processing(
                stage = ReconstructionStage.Texturing,
                stageProgress = 0.4f,
                overallProgress = 0.71f,
                etaMillis = 42_000,
            ),
            ProjectStatus.Ready(refiningSplats = false),
            ProjectStatus.Failed(
                stage = ReconstructionStage.Fusion,
                reason = "ran out of voxel blocks",
                resumableFrom = ReconstructionStage.Depth,
            ),
            ProjectStatus.Cancelled,
        )
        for (status in statuses) {
            assertThat(project(status).toEntity().toDomain().status).isEqualTo(status)
        }
    }

    @Test
    fun `status kind is a distinct column value per variant`() {
        val kinds = listOf(
            ProjectStatus.Capturing(0),
            ProjectStatus.Captured,
            ProjectStatus.Queued(1),
            ProjectStatus.Processing(ReconstructionStage.Ingest, 0f, 0f),
            ProjectStatus.Ready(),
            ProjectStatus.Failed(ReconstructionStage.Ingest, "x", null),
            ProjectStatus.Cancelled,
        ).map { project(it).toEntity().statusKind }
        assertThat(kinds).containsNoDuplicates()
    }

    @Test
    fun `an unreadable status degrades to cancelled rather than throwing`() {
        // A row written by a newer build, or truncated by a crash. One unopenable project is a
        // far better failure than a library that will not load at all.
        val broken = project(ProjectStatus.Captured).toEntity().copy(statusJson = "{ not json")
        assertThat(broken.toDomain().status).isEqualTo(ProjectStatus.Cancelled)
    }

    @Test
    fun `an unknown output kind is dropped rather than failing the row`() {
        val entity = project(ProjectStatus.Captured).toEntity().copy(outputs = "Mesh,Hologram")
        assertThat(entity.toDomain().outputs).containsExactly(OutputKind.Mesh)
    }

    @Test
    fun `an unknown enum name falls back to the default`() {
        val entity = project(ProjectStatus.Captured).toEntity().copy(quality = "Ludicrous")
        assertThat(entity.toDomain().quality).isEqualTo(QualityPreset.Balanced)
    }

    @Test
    fun `measurements round trip for each kind`() {
        val measurements = listOf(
            Measurement.Distance("m1", "Doorway", 0f, 0f, 0f, 0.9f, 0f, 0f),
            Measurement.Height("m2", "Ceiling", baseY = 0f, topY = 2.44f),
            Measurement.Area(
                "m3",
                "Floor",
                listOf(Point3(0f, 0f, 0f), Point3(4f, 0f, 0f), Point3(4f, 0f, 3f)),
            ),
        )
        for (measurement in measurements) {
            val restored = measurement.toEntity("p1", createdAtMillis = 5).toDomain()
            assertThat(restored).isEqualTo(measurement)
        }
    }

    @Test
    fun `an unknown measurement kind yields null instead of a crash`() {
        val entity = Measurement.Height("m", "h", 0f, 1f)
            .toEntity("p1", 0)
            .copy(kind = "volume")
        assertThat(entity.toDomain()).isNull()
    }
}
