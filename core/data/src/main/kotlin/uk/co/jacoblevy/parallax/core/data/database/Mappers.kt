package uk.co.jacoblevy.parallax.core.data.database

import kotlinx.serialization.json.Json
import uk.co.jacoblevy.parallax.core.model.Annotation
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.CaptureSummary
import uk.co.jacoblevy.parallax.core.model.Folder
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.ModelMeasurements
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.Point3
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset

/**
 * Entity to domain mapping.
 *
 * The database stores a few fields as JSON because their shape is a sealed hierarchy or a set,
 * neither of which maps onto a column usefully. Everything the library screen sorts or filters
 * on is a real column, so no query has to parse JSON to do its job.
 *
 * All of these conversions are lenient: a row written by a newer build, or corrupted by a
 * half-finished write, degrades to a sensible default rather than throwing. A library that
 * refuses to open because one project of forty has an unreadable status field would be a much
 * worse failure than one project showing as "Cancelled".
 */
internal val projectJson: Json = Json {
    ignoreUnknownKeys = true
    encodeDefaults = true
}

/** Discriminator written to `status_kind`. Stable: queries and indices depend on it. */
internal fun ProjectStatus.kind(): String = when (this) {
    is ProjectStatus.Capturing -> "capturing"
    is ProjectStatus.Captured -> "captured"
    is ProjectStatus.Queued -> "queued"
    is ProjectStatus.Processing -> "processing"
    is ProjectStatus.Ready -> "ready"
    is ProjectStatus.Failed -> "failed"
    ProjectStatus.Cancelled -> "cancelled"
}

private inline fun <reified T : Enum<T>> String.toEnumOr(fallback: T): T =
    enumValues<T>().firstOrNull { it.name == this } ?: fallback

private fun encodeOutputs(outputs: Set<OutputKind>): String = outputs.joinToString(",") { it.name }

private fun decodeOutputs(raw: String): Set<OutputKind> =
    if (raw.isBlank()) {
        emptySet()
    } else {
        raw.split(',')
            .mapNotNull { name -> enumValues<OutputKind>().firstOrNull { it.name == name.trim() } }
            .toSet()
    }

fun ProjectEntity.toDomain(): Project = Project(
    id = id,
    name = name,
    captureMode = captureMode.toEnumOr(CaptureMode.Object),
    createdAtMillis = createdAtMillis,
    updatedAtMillis = updatedAtMillis,
    status = runCatching { projectJson.decodeFromString<ProjectStatus>(statusJson) }
        .getOrElse { ProjectStatus.Cancelled },
    folderId = folderId,
    quality = quality.toEnumOr(QualityPreset.Balanced),
    capture = runCatching { projectJson.decodeFromString<CaptureSummary>(captureJson) }
        .getOrElse { CaptureSummary() },
    outputs = decodeOutputs(outputs),
    measurements = measurementsJson?.let {
        runCatching { projectJson.decodeFromString<ModelMeasurements>(it) }.getOrNull()
    },
    storageBytes = storageBytes,
    favourite = favourite,
    notes = notes,
)

fun Project.toEntity(): ProjectEntity = ProjectEntity(
    id = id,
    name = name,
    captureMode = captureMode.name,
    createdAtMillis = createdAtMillis,
    updatedAtMillis = updatedAtMillis,
    statusKind = status.kind(),
    statusJson = projectJson.encodeToString(status),
    folderId = folderId,
    quality = quality.name,
    captureJson = projectJson.encodeToString(capture),
    outputs = encodeOutputs(outputs),
    measurementsJson = measurements?.let { projectJson.encodeToString(it) },
    storageBytes = storageBytes,
    favourite = favourite,
    notes = notes,
)

fun FolderWithCount.toDomain(): Folder = Folder(
    id = id,
    name = name,
    createdAtMillis = createdAtMillis,
    projectCount = projectCount,
)

fun FolderEntity.toDomain(): Folder = Folder(id, name, createdAtMillis)

fun AnnotationEntity.toDomain(): Annotation = Annotation(
    id = id,
    title = title,
    body = body,
    position = Point3(x, y, z),
    normal = if (normalX != null && normalY != null && normalZ != null) {
        Point3(normalX, normalY, normalZ)
    } else {
        null
    },
    colorArgb = colorArgb,
    createdAtMillis = createdAtMillis,
)

fun Annotation.toEntity(projectId: String): AnnotationEntity = AnnotationEntity(
    id = id,
    projectId = projectId,
    title = title,
    body = body,
    x = position.x,
    y = position.y,
    z = position.z,
    normalX = normal?.x,
    normalY = normal?.y,
    normalZ = normal?.z,
    colorArgb = colorArgb,
    createdAtMillis = createdAtMillis,
)

/**
 * Measurement kinds, written to the `kind` column.
 *
 * The geometry itself is JSON: a distance needs six floats, a height two and an area a list, so
 * modelling them as columns would mean six nullable ones and no way to validate which are set.
 */
private const val KIND_DISTANCE = "distance"
private const val KIND_HEIGHT = "height"
private const val KIND_AREA = "area"

fun MeasurementEntity.toDomain(): Measurement? = runCatching {
    when (kind) {
        KIND_DISTANCE -> projectJson.decodeFromString<Measurement.Distance>(geometryJson)
        KIND_HEIGHT -> projectJson.decodeFromString<Measurement.Height>(geometryJson)
        KIND_AREA -> projectJson.decodeFromString<Measurement.Area>(geometryJson)
        else -> null
    }
}.getOrNull()

fun Measurement.toEntity(projectId: String, createdAtMillis: Long): MeasurementEntity {
    val kind = when (this) {
        is Measurement.Distance -> KIND_DISTANCE
        is Measurement.Height -> KIND_HEIGHT
        is Measurement.Area -> KIND_AREA
    }
    val geometry = when (this) {
        is Measurement.Distance -> projectJson.encodeToString(this)
        is Measurement.Height -> projectJson.encodeToString(this)
        is Measurement.Area -> projectJson.encodeToString(this)
    }
    return MeasurementEntity(
        id = id,
        projectId = projectId,
        kind = kind,
        label = label,
        geometryJson = geometry,
        createdAtMillis = createdAtMillis,
    )
}

/** Copy a measurement with a new id, which import needs because ids are table-wide. */
fun Measurement.withId(id: String): Measurement = when (this) {
    is Measurement.Distance -> copy(id = id)
    is Measurement.Height -> copy(id = id)
    is Measurement.Area -> copy(id = id)
}
