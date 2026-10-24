package uk.co.jacoblevy.parallax.core.data.storage

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.ParallaxException
import uk.co.jacoblevy.parallax.core.model.Annotation
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.Project
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream
import javax.inject.Inject
import javax.inject.Singleton

/** A `.parallax` file's metadata entry. */
@Serializable
data class BundleMetadata(
    val version: Int = VERSION,
    val exportedAtMillis: Long,
    val project: Project,
    val annotations: List<Annotation> = emptyList(),
    val measurements: List<Measurement> = emptyList(),
    val includesRawFrames: Boolean = false,
) {
    companion object {
        /**
         * Bundle format version.
         *
         * Import accepts anything up to [VERSION]; a newer bundle is rejected with a message
         * naming the version rather than being partially read, because a silently incomplete
         * import of someone's only copy of a scan is the worst possible outcome.
         */
        const val VERSION = 1
    }
}

/**
 * Reads and writes `.parallax` bundles: a project, entirely self-contained, as a single file.
 *
 * The container is an ordinary ZIP, so a bundle can be opened with any unzip tool and the model
 * inside recovered even if this app is gone. That is deliberate: a local-only app with no cloud
 * backup owes the user a format they are not locked into.
 */
@Singleton
class ProjectBundle @Inject constructor(private val storage: ProjectStorage) {

    private val json = Json { prettyPrint = true; encodeDefaults = true; ignoreUnknownKeys = true }

    /**
     * Write [project] and its outputs to [out].
     *
     * Raw frames are excluded unless asked for: they are the bulk of the size and are of no use
     * to anyone who is not going to re-process the scan.
     */
    fun write(
        out: OutputStream,
        project: Project,
        annotations: List<Annotation>,
        measurements: List<Measurement>,
        includeRawFrames: Boolean,
        nowMillis: Long,
    ) {
        val id = project.id
        val metadata = BundleMetadata(
            exportedAtMillis = nowMillis,
            project = project,
            annotations = annotations,
            measurements = measurements,
            includesRawFrames = includeRawFrames,
        )
        ZipOutputStream(out.buffered()).use { zip ->
            zip.putEntry(METADATA) { it.write(json.encodeToString(metadata).toByteArray()) }
            storage.manifestFile(id).takeIf { it.isFile }?.let {
                zip.putFile(ProjectStorage.MANIFEST, it)
            }
            storage.thumbnailFile(id).takeIf { it.isFile }?.let {
                zip.putFile(ProjectStorage.THUMBNAIL, it)
            }
            zip.putTree(storage.outputsDir(id), ProjectStorage.OUTPUTS)
            if (includeRawFrames) {
                zip.putTree(storage.framesDir(id), ProjectStorage.FRAMES)
                zip.putTree(storage.depthDir(id), ProjectStorage.DEPTH)
            }
        }
    }

    /**
     * Read a bundle from [input], creating the project directory for [newProjectId].
     *
     * The id is reassigned by the caller rather than taken from the bundle so that importing a
     * bundle exported from this same device does not overwrite the original. The returned
     * project carries the new id and a fresh timestamp; everything else is as exported.
     */
    fun read(input: InputStream, newProjectId: String, nowMillis: Long): BundleMetadata {
        val destination = storage.create(newProjectId)
        // A half-extracted project would appear in the library as a scan that cannot be
        // opened, so any failure unwinds the directory completely before reporting why.
        val metadata = try {
            extract(input, destination).also(::requireReadableVersion)
        } catch (failure: Exception) {
            storage.delete(newProjectId)
            throw failure as? ParallaxException
                ?: invalid(failure.message ?: failure::class.simpleName.orEmpty())
        }
        return metadata.copy(
            project = metadata.project.copy(
                id = newProjectId,
                updatedAtMillis = nowMillis,
                storageBytes = storage.sizeOf(newProjectId),
            ),
        )
    }

    private fun requireReadableVersion(metadata: BundleMetadata) {
        if (metadata.version > BundleMetadata.VERSION) {
            throw invalid(
                "this bundle was written by a newer version of Parallax " +
                    "(format ${metadata.version}, this build reads ${BundleMetadata.VERSION})",
            )
        }
    }

    /** Unpack every entry into [destination], returning the metadata entry. */
    private fun extract(input: InputStream, destination: File): BundleMetadata {
        var metadata: BundleMetadata? = null
        ZipInputStream(input.buffered()).use { zip ->
            var entry = zip.nextEntry
            while (entry != null) {
                if (!entry.isDirectory) {
                    metadata = readEntry(zip, destination, entry.name) ?: metadata
                }
                zip.closeEntry()
                entry = zip.nextEntry
            }
        }
        return metadata ?: throw invalid("no $METADATA entry")
    }

    /** Returns the metadata when [name] is the metadata entry, otherwise writes the file out. */
    private fun readEntry(
        zip: ZipInputStream,
        destination: File,
        name: String,
    ): BundleMetadata? {
        if (name == METADATA) {
            return json.decodeFromString<BundleMetadata>(zip.readBytes().decodeToString())
        }
        val target = resolveSafely(destination, name)
        target.parentFile?.mkdirs()
        target.outputStream().use { zip.copyTo(it) }
        return null
    }

    private fun invalid(detail: String) = ParallaxException(ParallaxError.BundleInvalid(detail))

    /**
     * Resolve an entry name inside [root], refusing anything that would escape it.
     *
     * A ZIP entry name is attacker-controlled data -- a bundle can arrive from anywhere through
     * the share sheet -- and `../../../databases/parallax.db` is a real attack, so the resolved
     * path is checked against the destination rather than the name being inspected for `..`.
     */
    private fun resolveSafely(root: File, entryName: String): File {
        val target = File(root, entryName).canonicalFile
        val base = root.canonicalFile
        if (target != base && !target.path.startsWith(base.path + File.separator)) {
            throw invalid("entry \"$entryName\" points outside the bundle")
        }
        return target
    }

    private inline fun ZipOutputStream.putEntry(name: String, write: (ZipOutputStream) -> Unit) {
        putNextEntry(ZipEntry(name))
        write(this)
        closeEntry()
    }

    private fun ZipOutputStream.putFile(name: String, file: File) {
        putEntry(name) { out -> file.inputStream().use { it.copyTo(out) } }
    }

    private fun ZipOutputStream.putTree(dir: File, prefix: String) {
        val children = dir.listFiles() ?: return
        for (file in children.sortedBy { it.name }) {
            if (file.isDirectory) {
                putTree(file, "$prefix/${file.name}")
            } else {
                putFile("$prefix/${file.name}", file)
            }
        }
    }

    companion object {
        const val METADATA = "bundle.json"
        const val EXTENSION = "parallax"
    }
}
