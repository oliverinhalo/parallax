package uk.co.jacoblevy.parallax.core.data.database

import androidx.room.Dao
import androidx.room.Delete
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import androidx.room.Upsert
import kotlinx.coroutines.flow.Flow

@Dao
interface ProjectDao {

    @Query("SELECT * FROM projects ORDER BY favourite DESC, updated_at DESC")
    fun observeAll(): Flow<List<ProjectEntity>>

    @Query("SELECT * FROM projects WHERE id = :id")
    fun observe(id: String): Flow<ProjectEntity?>

    @Query("SELECT * FROM projects WHERE id = :id")
    suspend fun get(id: String): ProjectEntity?

    @Query("SELECT * FROM projects WHERE folder_id = :folderId ORDER BY updated_at DESC")
    fun observeInFolder(folderId: String): Flow<List<ProjectEntity>>

    @Query("SELECT * FROM projects WHERE folder_id IS NULL ORDER BY updated_at DESC")
    fun observeUnfiled(): Flow<List<ProjectEntity>>

    /**
     * Full-text-ish search over name and notes.
     *
     * LIKE with a leading wildcard cannot use an index, but the library is in the low thousands
     * of rows at most and this keeps the schema free of an FTS table that would then have to be
     * kept in sync on every edit.
     */
    @Query(
        """
        SELECT * FROM projects
        WHERE name LIKE '%' || :query || '%' OR notes LIKE '%' || :query || '%'
        ORDER BY favourite DESC, updated_at DESC
        """,
    )
    fun search(query: String): Flow<List<ProjectEntity>>

    @Query("SELECT * FROM projects WHERE status_kind = :statusKind ORDER BY updated_at DESC")
    fun observeByStatus(statusKind: String): Flow<List<ProjectEntity>>

    /** Every project id, for reconciling the database against the directories on disk. */
    @Query("SELECT id FROM projects")
    suspend fun allIds(): List<String>

    @Query("SELECT COUNT(*) FROM projects")
    fun observeCount(): Flow<Int>

    @Query("SELECT COALESCE(SUM(storage_bytes), 0) FROM projects")
    fun observeTotalStorageBytes(): Flow<Long>

    @Upsert
    suspend fun upsert(project: ProjectEntity)

    @Upsert
    suspend fun upsertAll(projects: List<ProjectEntity>)

    @Query("UPDATE projects SET name = :name, updated_at = :updatedAt WHERE id = :id")
    suspend fun rename(id: String, name: String, updatedAt: Long)

    @Query("UPDATE projects SET favourite = :favourite, updated_at = :updatedAt WHERE id = :id")
    suspend fun setFavourite(id: String, favourite: Boolean, updatedAt: Long)

    @Query("UPDATE projects SET folder_id = :folderId, updated_at = :updatedAt WHERE id = :id")
    suspend fun moveToFolder(id: String, folderId: String?, updatedAt: Long)

    @Query(
        """
        UPDATE projects
        SET status_kind = :statusKind, status_json = :statusJson, updated_at = :updatedAt
        WHERE id = :id
        """,
    )
    suspend fun updateStatus(id: String, statusKind: String, statusJson: String, updatedAt: Long)

    @Query("UPDATE projects SET storage_bytes = :bytes WHERE id = :id")
    suspend fun updateStorageBytes(id: String, bytes: Long)

    @Query("DELETE FROM projects WHERE id = :id")
    suspend fun delete(id: String)
}

@Dao
interface FolderDao {

    /**
     * Folders with a live project count.
     *
     * A LEFT JOIN so an empty folder still appears: filtering it out would make a
     * just-created folder vanish, which looks like the creation failed.
     */
    @Query(
        """
        SELECT f.id, f.name, f.created_at AS createdAtMillis,
               COUNT(p.id) AS projectCount
        FROM folders f
        LEFT JOIN projects p ON p.folder_id = f.id
        GROUP BY f.id
        ORDER BY f.name COLLATE NOCASE ASC
        """,
    )
    fun observeAllWithCounts(): Flow<List<FolderWithCount>>

    @Query("SELECT * FROM folders WHERE id = :id")
    suspend fun get(id: String): FolderEntity?

    @Insert(onConflict = OnConflictStrategy.ABORT)
    suspend fun insert(folder: FolderEntity)

    @Query("UPDATE folders SET name = :name WHERE id = :id")
    suspend fun rename(id: String, name: String)

    @Delete
    suspend fun delete(folder: FolderEntity)

    @Query("DELETE FROM folders WHERE id = :id")
    suspend fun delete(id: String)
}

data class FolderWithCount(
    val id: String,
    val name: String,
    val createdAtMillis: Long,
    val projectCount: Int,
)

@Dao
interface AnnotationDao {
    @Query("SELECT * FROM annotations WHERE project_id = :projectId ORDER BY created_at ASC")
    fun observeForProject(projectId: String): Flow<List<AnnotationEntity>>

    @Query("SELECT * FROM annotations WHERE project_id = :projectId ORDER BY created_at ASC")
    suspend fun getForProject(projectId: String): List<AnnotationEntity>

    @Upsert
    suspend fun upsert(annotation: AnnotationEntity)

    @Upsert
    suspend fun upsertAll(annotations: List<AnnotationEntity>)

    @Query("DELETE FROM annotations WHERE id = :id")
    suspend fun delete(id: String)

    @Query("DELETE FROM annotations WHERE project_id = :projectId")
    suspend fun deleteForProject(projectId: String)
}

@Dao
interface MeasurementDao {
    @Query("SELECT * FROM measurements WHERE project_id = :projectId ORDER BY created_at ASC")
    fun observeForProject(projectId: String): Flow<List<MeasurementEntity>>

    @Query("SELECT * FROM measurements WHERE project_id = :projectId ORDER BY created_at ASC")
    suspend fun getForProject(projectId: String): List<MeasurementEntity>

    @Upsert
    suspend fun upsert(measurement: MeasurementEntity)

    @Upsert
    suspend fun upsertAll(measurements: List<MeasurementEntity>)

    @Query("DELETE FROM measurements WHERE id = :id")
    suspend fun delete(id: String)

    @Query("DELETE FROM measurements WHERE project_id = :projectId")
    suspend fun deleteForProject(projectId: String)
}

@Dao
interface ProcessingJobDao {

    /** The queue, oldest first, which is the order work is taken in. */
    @Query("SELECT * FROM processing_jobs WHERE finished_at IS NULL ORDER BY queued_at ASC")
    fun observeQueue(): Flow<List<ProcessingJobEntity>>

    @Query("SELECT * FROM processing_jobs WHERE finished_at IS NULL ORDER BY queued_at ASC")
    suspend fun pending(): List<ProcessingJobEntity>

    @Query("SELECT * FROM processing_jobs WHERE project_id = :projectId")
    suspend fun forProject(projectId: String): ProcessingJobEntity?

    @Query("SELECT COUNT(*) FROM processing_jobs WHERE finished_at IS NULL")
    fun observePendingCount(): Flow<Int>

    /**
     * Position of a project in the queue, 1-based, or 0 if it is not queued. Computed in SQL so
     * the UI does not have to hold the whole queue to label one row.
     */
    @Query(
        """
        SELECT COUNT(*) FROM processing_jobs
        WHERE finished_at IS NULL
          AND queued_at <= (SELECT queued_at FROM processing_jobs WHERE project_id = :projectId)
        """,
    )
    suspend fun queuePosition(projectId: String): Int

    @Upsert
    suspend fun upsert(job: ProcessingJobEntity)

    @Query("UPDATE processing_jobs SET started_at = :startedAt WHERE id = :id")
    suspend fun markStarted(id: String, startedAt: Long)

    @Query(
        """
        UPDATE processing_jobs
        SET finished_at = :finishedAt, failure_reason = :failureReason
        WHERE id = :id
        """,
    )
    suspend fun markFinished(id: String, finishedAt: Long, failureReason: String?)

    @Query("UPDATE processing_jobs SET completed_stage = :stage WHERE id = :id")
    suspend fun updateCompletedStage(id: String, stage: Int)

    @Query("UPDATE processing_jobs SET attempt_count = attempt_count + 1 WHERE id = :id")
    suspend fun incrementAttempts(id: String)

    @Query("DELETE FROM processing_jobs WHERE id = :id")
    suspend fun delete(id: String)

    @Query("DELETE FROM processing_jobs WHERE project_id = :projectId")
    suspend fun deleteForProject(projectId: String)

    @Query("DELETE FROM processing_jobs WHERE finished_at IS NOT NULL")
    suspend fun clearFinished()

    /**
     * Reset jobs that were running when the process died.
     *
     * Called once at startup. Without it a job whose process was killed mid-stage stays
     * "started" forever and the queue never advances past it. The completed-stage counter is
     * kept, so the retry resumes from its checkpoint rather than starting over.
     */
    @Query(
        """
        UPDATE processing_jobs SET started_at = NULL
        WHERE started_at IS NOT NULL AND finished_at IS NULL
        """,
    )
    suspend fun recoverInterrupted()
}
