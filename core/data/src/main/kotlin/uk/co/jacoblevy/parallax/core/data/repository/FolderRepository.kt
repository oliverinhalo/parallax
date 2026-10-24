package uk.co.jacoblevy.parallax.core.data.repository

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext
import uk.co.jacoblevy.parallax.core.common.di.IoDispatcher
import uk.co.jacoblevy.parallax.core.data.database.FolderDao
import uk.co.jacoblevy.parallax.core.data.database.FolderEntity
import uk.co.jacoblevy.parallax.core.data.database.toDomain
import uk.co.jacoblevy.parallax.core.model.Folder
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class FolderRepository @Inject constructor(
    private val dao: FolderDao,
    private val time: TimeSource,
    @IoDispatcher private val io: CoroutineDispatcher,
) {
    fun observeAll(): Flow<List<Folder>> =
        dao.observeAllWithCounts().map { rows -> rows.map { it.toDomain() } }

    suspend fun create(id: String, name: String): Folder = withContext(io) {
        val folder = FolderEntity(
            id = id,
            name = name.trim().ifEmpty { "New folder" },
            createdAtMillis = time.nowMillis(),
        )
        dao.insert(folder)
        folder.toDomain()
    }

    suspend fun rename(id: String, name: String) = withContext(io) {
        dao.rename(id, name.trim().ifEmpty { "New folder" })
    }

    /** Deleting a folder leaves its projects in place, unfiled: the foreign key sets them null. */
    suspend fun delete(id: String) = withContext(io) { dao.delete(id) }
}
