package uk.co.jacoblevy.parallax.core.data.database

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.ForeignKey
import androidx.room.Index
import androidx.room.PrimaryKey

/**
 * Project metadata.
 *
 * Only metadata lives in the database. Frames, meshes, textures and checkpoints are files on
 * disk referenced by the project id: SQLite is a poor container for hundreds of megabytes of
 * binary data, and keeping them separate means the reconstruction engine can read and write
 * them directly from C++ without going through the database at all.
 */
@Entity(
    tableName = "projects",
    foreignKeys = [
        ForeignKey(
            entity = FolderEntity::class,
            parentColumns = ["id"],
            childColumns = ["folder_id"],
            // A deleted folder must not take its projects with it; they fall back to the root.
            onDelete = ForeignKey.SET_NULL,
        ),
    ],
    indices = [
        Index("folder_id"),
        Index("updated_at"),
        // The library's default sort and its status filter, so neither needs a table scan.
        Index(value = ["status_kind", "updated_at"]),
    ],
)
data class ProjectEntity(
    @PrimaryKey val id: String,
    val name: String,
    @ColumnInfo(name = "capture_mode") val captureMode: String,
    @ColumnInfo(name = "created_at") val createdAtMillis: Long,
    @ColumnInfo(name = "updated_at") val updatedAtMillis: Long,
    /**
     * Discriminator for the status, stored separately from [statusJson] so the library can
     * filter and sort on it in SQL. Keeping only the JSON would force every query to
     * deserialise every row.
     */
    @ColumnInfo(name = "status_kind") val statusKind: String,
    @ColumnInfo(name = "status_json") val statusJson: String,
    @ColumnInfo(name = "folder_id") val folderId: String?,
    val quality: String,
    @ColumnInfo(name = "capture_json") val captureJson: String,
    @ColumnInfo(name = "outputs") val outputs: String,
    @ColumnInfo(name = "measurements_json") val measurementsJson: String?,
    @ColumnInfo(name = "edits_json") val editsJson: String?,
    @ColumnInfo(name = "storage_bytes") val storageBytes: Long,
    val favourite: Boolean,
    val notes: String,
)

@Entity(tableName = "folders", indices = [Index(value = ["name"], unique = true)])
data class FolderEntity(
    @PrimaryKey val id: String,
    val name: String,
    @ColumnInfo(name = "created_at") val createdAtMillis: Long,
)

/** A measurement or annotation the user placed on a model. */
@Entity(
    tableName = "annotations",
    foreignKeys = [
        ForeignKey(
            entity = ProjectEntity::class,
            parentColumns = ["id"],
            childColumns = ["project_id"],
            // Annotations are meaningless without their project.
            onDelete = ForeignKey.CASCADE,
        ),
    ],
    indices = [Index("project_id")],
)
data class AnnotationEntity(
    @PrimaryKey val id: String,
    @ColumnInfo(name = "project_id") val projectId: String,
    val title: String,
    val body: String,
    val x: Float,
    val y: Float,
    val z: Float,
    @ColumnInfo(name = "normal_x") val normalX: Float?,
    @ColumnInfo(name = "normal_y") val normalY: Float?,
    @ColumnInfo(name = "normal_z") val normalZ: Float?,
    @ColumnInfo(name = "color_argb") val colorArgb: Int,
    @ColumnInfo(name = "created_at") val createdAtMillis: Long,
)

@Entity(
    tableName = "measurements",
    foreignKeys = [
        ForeignKey(
            entity = ProjectEntity::class,
            parentColumns = ["id"],
            childColumns = ["project_id"],
            onDelete = ForeignKey.CASCADE,
        ),
    ],
    indices = [Index("project_id")],
)
data class MeasurementEntity(
    @PrimaryKey val id: String,
    @ColumnInfo(name = "project_id") val projectId: String,
    val kind: String,
    val label: String,
    /** Geometry, as JSON: the shape differs per [kind] and is not worth four nullable columns. */
    @ColumnInfo(name = "geometry_json") val geometryJson: String,
    @ColumnInfo(name = "created_at") val createdAtMillis: Long,
)

/**
 * A queued or running processing job.
 *
 * Persisted rather than held in memory because the queue has to survive process death: a user
 * who queues four scans and locks the phone expects to find four finished models, not an empty
 * queue.
 */
@Entity(
    tableName = "processing_jobs",
    foreignKeys = [
        ForeignKey(
            entity = ProjectEntity::class,
            parentColumns = ["id"],
            childColumns = ["project_id"],
            onDelete = ForeignKey.CASCADE,
        ),
    ],
    indices = [Index(value = ["project_id"], unique = true), Index("queued_at")],
)
data class ProcessingJobEntity(
    @PrimaryKey val id: String,
    @ColumnInfo(name = "project_id") val projectId: String,
    val quality: String,
    @ColumnInfo(name = "queued_at") val queuedAtMillis: Long,
    @ColumnInfo(name = "started_at") val startedAtMillis: Long?,
    @ColumnInfo(name = "finished_at") val finishedAtMillis: Long?,
    /** Ordinal of the last completed stage, so a resumed job knows where it was. */
    @ColumnInfo(name = "completed_stage") val completedStage: Int,
    @ColumnInfo(name = "failure_reason") val failureReason: String?,
    @ColumnInfo(name = "attempt_count") val attemptCount: Int,
)
