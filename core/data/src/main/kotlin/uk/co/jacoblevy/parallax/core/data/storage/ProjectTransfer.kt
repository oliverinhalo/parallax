package uk.co.jacoblevy.parallax.core.data.storage

import android.content.Context
import android.net.Uri
import androidx.annotation.VisibleForTesting
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.withContext
import uk.co.jacoblevy.parallax.core.common.di.IoDispatcher
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.ParallaxException
import uk.co.jacoblevy.parallax.core.data.repository.IdGenerator
import uk.co.jacoblevy.parallax.core.data.repository.ProjectRepository
import uk.co.jacoblevy.parallax.core.model.Project
import javax.inject.Inject
import java.io.File
import javax.inject.Singleton

/**
 * Moves `.parallax` bundles between the app and the documents the user picked.
 *
 * This is the only place that touches a [Uri]: the repository works in streams, which keeps it
 * testable without a `ContentResolver`, and keeps the Storage Access Framework at the edge.
 */
@Singleton
class ProjectTransfer @Inject constructor(
    @ApplicationContext private val context: Context,
    private val projects: ProjectRepository,
    private val ids: IdGenerator,
    private val storage: ProjectStorage,
    @IoDispatcher private val io: CoroutineDispatcher,
) {

    /** Import a bundle the user picked. The new project gets a fresh id. */
    suspend fun import(uri: Uri): Project = withContext(io) {
        val stream = context.contentResolver.openInputStream(uri)
            ?: throw ParallaxException(ParallaxError.BundleInvalid("could not open the file"))
        stream.use { projects.importBundle(it, ids.newId()) }
    }

    /** Write a bundle to a document the user picked. */
    suspend fun export(projectId: String, uri: Uri, includeRawFrames: Boolean) = withContext(io) {
        val stream = context.contentResolver.openOutputStream(uri)
            ?: throw ParallaxException(
                ParallaxError.StorageFailure(uri.toString(), "could not open the destination"),
            )
        stream.use { projects.exportBundle(projectId, it, includeRawFrames) }
    }

    /**
     * Stage a file for the share sheet.
     *
     * Sharing needs a `content://` URI from a `FileProvider`, and the provider is declared over
     * this one directory, so a share grant cannot reach anything else the app has stored.
     */
    @VisibleForTesting
    fun stagingFile(fileName: String): File = storage.sharedFile(fileName)

    /**
     * Copy a finished export into a document the user picked.
     *
     * Exports are written into the project first and copied out afterwards, so
     * that a failure part-way through leaves nothing half-written where the
     * user can see it, and a cancelled save dialog leaves nothing at all.
     */
    suspend fun copyToUri(source: File, uri: Uri) = withContext(io) {
        val stream = context.contentResolver.openOutputStream(uri)
            ?: throw ParallaxException(
                ParallaxError.StorageFailure(uri.toString(), "could not open the destination"),
            )
        stream.use { out -> source.inputStream().use { it.copyTo(out) } }
    }

    /** A suggested file name for a project's bundle, safe for any filesystem. */
    fun suggestedBundleName(project: Project): String {
        val base = project.name.map { if (it.isLetterOrDigit() || it == ' ') it else '-' }
            .joinToString("")
            .trim()
            .replace(Regex("\\s+"), "-")
            .ifEmpty { "parallax-scan" }
        return "$base.${ProjectBundle.EXTENSION}"
    }
}
