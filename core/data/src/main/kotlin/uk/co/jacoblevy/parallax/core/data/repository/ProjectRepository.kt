package uk.co.jacoblevy.parallax.core.data.repository

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext
import uk.co.jacoblevy.parallax.core.common.di.IoDispatcher
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.ParallaxException
import uk.co.jacoblevy.parallax.core.data.database.AnnotationDao
import uk.co.jacoblevy.parallax.core.data.database.MeasurementDao
import uk.co.jacoblevy.parallax.core.data.database.ProjectDao
import uk.co.jacoblevy.parallax.core.data.database.kind
import uk.co.jacoblevy.parallax.core.data.database.projectJson
import uk.co.jacoblevy.parallax.core.data.database.toDomain
import uk.co.jacoblevy.parallax.core.data.database.toEntity
import uk.co.jacoblevy.parallax.core.data.database.withId
import uk.co.jacoblevy.parallax.core.data.storage.BundleMetadata
import uk.co.jacoblevy.parallax.core.data.storage.CaptureManifest
import uk.co.jacoblevy.parallax.core.data.storage.ProjectBundle
import uk.co.jacoblevy.parallax.core.data.storage.ProjectStorage
import uk.co.jacoblevy.parallax.core.model.Annotation
import uk.co.jacoblevy.parallax.core.model.CaptureFrame
import uk.co.jacoblevy.parallax.core.model.CaptureMode
import uk.co.jacoblevy.parallax.core.model.CaptureSummary
import uk.co.jacoblevy.parallax.core.model.LibraryQuery
import uk.co.jacoblevy.parallax.core.model.LibrarySort
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.ModelMeasurements
import uk.co.jacoblevy.parallax.core.model.OutputKind
import uk.co.jacoblevy.parallax.core.model.Project
import uk.co.jacoblevy.parallax.core.model.ProjectStatus
import uk.co.jacoblevy.parallax.core.model.QualityPreset
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import javax.inject.Inject
import javax.inject.Singleton

/** Supplies the current time, so repository behaviour is testable without waiting for a clock. */
fun interface TimeSource {
    fun nowMillis(): Long
}

/**
 * Projects, as the rest of the app sees them.
 *
 * The database and the filesystem are two stores that have to agree, and this is the only place
 * that writes both. Ordering matters in one direction: the directory is created before the row
 * and deleted after it, so a crash at any point leaves at worst an orphaned directory --
 * recoverable by [reconcile] -- rather than a library entry pointing at nothing, which the user
 * would see as a scan that cannot be opened.
 */
@Singleton
class ProjectRepository @Inject constructor(
    private val projectDao: ProjectDao,
    private val annotationDao: AnnotationDao,
    private val measurementDao: MeasurementDao,
    private val storage: ProjectStorage,
    private val bundles: ProjectBundle,
    private val time: TimeSource,
    @IoDispatcher private val io: CoroutineDispatcher,
) {

    fun observeAll(): Flow<List<Project>> =
        projectDao.observeAll().map { rows -> rows.map { it.toDomain() } }

    fun observe(id: String): Flow<Project?> = projectDao.observe(id).map { it?.toDomain() }

    /**
     * The library's list, filtered and sorted.
     *
     * Sorting happens here rather than in SQL because the sort is one of six options and the
     * result set is at most a few hundred rows; six query variants would be more code and more
     * places for the filter logic to drift apart.
     */
    fun observeLibrary(query: Flow<LibraryQuery>): Flow<List<Project>> =
        combine(projectDao.observeAll(), query) { rows, filter ->
            rows.asSequence()
                .map { it.toDomain() }
                .filter { it.folderId == filter.folderId || filter.folderId == null }
                .filter { filter.modeFilter == null || it.captureMode == filter.modeFilter }
                .filter { project -> !filter.favouritesOnly || project.favourite }
                .filter { project -> matchesSearch(project, filter.search) }
                .sortedWith(comparatorFor(filter.sort))
                .toList()
        }

    fun observeTotalStorageBytes(): Flow<Long> = projectDao.observeTotalStorageBytes()

    fun observeCount(): Flow<Int> = projectDao.observeCount()

    suspend fun get(id: String): Project? = withContext(io) { projectDao.get(id)?.toDomain() }

    suspend fun require(id: String): Project =
        get(id) ?: throw ParallaxException(ParallaxError.ProjectNotFound(id))

    /** Create an empty project and its directory, ready for capture to write frames into. */
    suspend fun create(
        id: String,
        name: String,
        mode: CaptureMode,
        quality: QualityPreset,
        folderId: String? = null,
    ): Project = withContext(io) {
        storage.create(id)
        val now = time.nowMillis()
        val project = Project(
            id = id,
            name = name,
            captureMode = mode,
            createdAtMillis = now,
            updatedAtMillis = now,
            status = ProjectStatus.Capturing(frameCount = 0),
            folderId = folderId,
            quality = quality,
        )
        projectDao.upsert(project.toEntity())
        project
    }

    suspend fun upsert(project: Project) = withContext(io) {
        projectDao.upsert(project.copy(updatedAtMillis = time.nowMillis()).toEntity())
    }

    suspend fun rename(id: String, name: String) = withContext(io) {
        projectDao.rename(id, name.trim().ifEmpty { "Untitled scan" }, time.nowMillis())
    }

    suspend fun setFavourite(id: String, favourite: Boolean) = withContext(io) {
        projectDao.setFavourite(id, favourite, time.nowMillis())
    }

    suspend fun moveToFolder(id: String, folderId: String?) = withContext(io) {
        projectDao.moveToFolder(id, folderId, time.nowMillis())
    }

    suspend fun setNotes(id: String, notes: String) = withContext(io) {
        val existing = projectDao.get(id) ?: return@withContext
        projectDao.upsert(existing.copy(notes = notes, updatedAtMillis = time.nowMillis()))
    }

    suspend fun updateStatus(id: String, status: ProjectStatus) = withContext(io) {
        projectDao.updateStatus(
            id = id,
            statusKind = status.kind(),
            statusJson = projectJson.encodeToString(status),
            updatedAt = time.nowMillis(),
        )
    }

    /** Record what capture produced, and the manifest the pipeline will read. */
    suspend fun finishCapture(
        id: String,
        summary: CaptureSummary,
        frames: List<CaptureFrame>,
    ): Project = withContext(io) {
        val existing = projectDao.get(id)?.toDomain()
            ?: throw ParallaxException(ParallaxError.ProjectNotFound(id))
        storage.writeManifest(
            projectId = id,
            manifest = CaptureManifest.from(
                projectId = id,
                mode = existing.captureMode,
                createdAtMillis = existing.createdAtMillis,
                frames = frames,
            ),
        )
        val updated = existing.copy(
            status = ProjectStatus.Captured,
            capture = summary,
            storageBytes = storage.sizeOf(id),
            updatedAtMillis = time.nowMillis(),
        )
        projectDao.upsert(updated.toEntity())
        updated
    }

    /** Record the outputs and measurements a completed reconstruction produced. */
    suspend fun recordOutputs(
        id: String,
        outputs: Set<OutputKind>,
        measurements: ModelMeasurements?,
        refiningSplats: Boolean = false,
    ): Project = withContext(io) {
        val existing = projectDao.get(id)?.toDomain()
            ?: throw ParallaxException(ParallaxError.ProjectNotFound(id))
        val updated = existing.copy(
            status = ProjectStatus.Ready(refiningSplats = refiningSplats),
            outputs = outputs,
            measurements = measurements ?: existing.measurements,
            storageBytes = storage.sizeOf(id),
            updatedAtMillis = time.nowMillis(),
        )
        projectDao.upsert(updated.toEntity())
        updated
    }

    /** Recompute a project's size from disk. Cheap enough to call after any write. */
    suspend fun refreshStorageBytes(id: String): Long = withContext(io) {
        val bytes = storage.sizeOf(id)
        projectDao.updateStorageBytes(id, bytes)
        bytes
    }

    suspend fun delete(id: String) = withContext(io) {
        // Row first: once it is gone the project cannot be opened, so a failure to delete the
        // directory degrades to wasted space that [reconcile] will clear, not a broken entry.
        projectDao.delete(id)
        storage.delete(id)
        Unit
    }

    suspend fun purgeIntermediates(id: String): Long = withContext(io) {
        val freed = storage.purgeIntermediates(id)
        projectDao.updateStorageBytes(id, storage.sizeOf(id))
        freed
    }

    /**
     * Bring the database and the filesystem back into agreement. Run once at startup.
     *
     * Returns the number of bytes reclaimed from orphaned directories.
     */
    suspend fun reconcile(): Long = withContext(io) {
        storage.deleteOrphans(projectDao.allIds().toSet())
    }

    // ---- annotations and measurements --------------------------------------

    fun observeAnnotations(projectId: String): Flow<List<Annotation>> =
        annotationDao.observeForProject(projectId).map { rows -> rows.map { it.toDomain() } }

    suspend fun upsertAnnotation(projectId: String, annotation: Annotation) = withContext(io) {
        annotationDao.upsert(
            annotation
                .copy(
                    createdAtMillis = annotation.createdAtMillis.takeIf { it > 0 }
                        ?: time.nowMillis(),
                )
                .toEntity(projectId),
        )
    }

    suspend fun deleteAnnotation(id: String) = withContext(io) { annotationDao.delete(id) }

    fun observeMeasurements(projectId: String): Flow<List<Measurement>> =
        measurementDao.observeForProject(projectId)
            .map { rows -> rows.mapNotNull { it.toDomain() } }

    suspend fun upsertMeasurement(projectId: String, measurement: Measurement) = withContext(io) {
        measurementDao.upsert(measurement.toEntity(projectId, time.nowMillis()))
    }

    suspend fun deleteMeasurement(id: String) = withContext(io) { measurementDao.delete(id) }

    // ---- bundles -----------------------------------------------------------

    suspend fun exportBundle(id: String, out: OutputStream, includeRawFrames: Boolean) =
        withContext(io) {
            val project = projectDao.get(id)?.toDomain()
                ?: throw ParallaxException(ParallaxError.ProjectNotFound(id))
            bundles.write(
                out = out,
                project = project,
                annotations = annotationDao.getForProject(id).map { it.toDomain() },
                measurements = measurementDao.getForProject(id).mapNotNull { it.toDomain() },
                includeRawFrames = includeRawFrames,
                nowMillis = time.nowMillis(),
            )
        }

    /**
     * Import a bundle as a new project.
     *
     * The id is new even when the bundle came from this device, so importing a backup never
     * overwrites whatever is in the library now.
     */
    suspend fun importBundle(input: InputStream, newId: String): Project = withContext(io) {
        val metadata: BundleMetadata = bundles.read(input, newId, time.nowMillis())
        val project = metadata.project.copy(
            // A re-imported project has no place in the importing device's folder tree.
            folderId = null,
            name = metadata.project.name,
        )
        projectDao.upsert(project.toEntity())
        // Annotation and measurement ids are primary keys for the whole table, not per project,
        // so reusing the bundle's ids when importing a bundle exported from this same device
        // would move the originals onto the copy instead of duplicating them. Prefixing with the
        // new project id keeps them unique and keeps the import deterministic.
        annotationDao.upsertAll(
            metadata.annotations.map { it.copy(id = "$newId:${it.id}").toEntity(newId) },
        )
        measurementDao.upsertAll(
            metadata.measurements.map { measurement ->
                measurement.withId("$newId:${measurement.id}").toEntity(newId, time.nowMillis())
            },
        )
        project
    }

    /** Path of a finished output, or null when the project does not have it. */
    fun outputFile(projectId: String, name: String): File? =
        storage.outputFile(projectId, name).takeIf { it.isFile }

    private fun matchesSearch(project: Project, search: String): Boolean {
        if (search.isBlank()) return true
        val needle = search.trim()
        return project.name.contains(needle, ignoreCase = true) ||
            project.notes.contains(needle, ignoreCase = true)
    }

    private fun comparatorFor(sort: LibrarySort): Comparator<Project> {
        // Favourites float to the top of every sort: the user pinned them deliberately, and a
        // sort order is about arranging the rest, not about discarding that choice.
        val favouritesFirst = compareByDescending<Project> { it.favourite }
        return when (sort) {
            LibrarySort.NewestFirst -> favouritesFirst.thenByDescending { it.updatedAtMillis }
            LibrarySort.OldestFirst -> favouritesFirst.thenBy { it.updatedAtMillis }
            LibrarySort.NameAscending ->
                favouritesFirst.thenBy(String.CASE_INSENSITIVE_ORDER) { it.name }
            LibrarySort.NameDescending ->
                favouritesFirst.thenBy(String.CASE_INSENSITIVE_ORDER.reversed()) { it.name }
            LibrarySort.LargestFirst -> favouritesFirst.thenByDescending { it.storageBytes }
            LibrarySort.SmallestFirst -> favouritesFirst.thenBy { it.storageBytes }
        }
    }
}
