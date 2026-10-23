import org.gradle.api.Project
import org.gradle.api.artifacts.VersionCatalog
import org.gradle.api.artifacts.VersionCatalogsExtension
import org.gradle.kotlin.dsl.getByType

/**
 * The root version catalog, reachable from precompiled script plugins.
 *
 * Deliberately *not* called `libs`: this type lives on the buildscript
 * classpath of every consuming module, where a `Project.libs` extension would
 * shadow Gradle's generated type-safe catalog accessor and break every
 * `libs.androidx.core.ktx` reference in the module build files.
 */
internal val Project.parallaxLibs: VersionCatalog
    get() = extensions.getByType<VersionCatalogsExtension>().named("libs")

/** Read an integer version (e.g. `compileSdk`) out of the catalog. */
internal fun VersionCatalog.int(alias: String): Int =
    findVersion(alias).orElseThrow { IllegalStateException("Missing version '$alias' in libs.versions.toml") }
        .requiredVersion.toInt()

/** Read a string version out of the catalog. */
internal fun VersionCatalog.str(alias: String): String =
    findVersion(alias).orElseThrow { IllegalStateException("Missing version '$alias' in libs.versions.toml") }
        .requiredVersion

/**
 * Java/Kotlin reserved words that can legitimately appear in a Gradle path but
 * cannot appear in a package name. `:engine:native` is the one that bites in
 * practice; the rest are listed so a future module cannot reintroduce the
 * problem.
 */
private val ReservedPackageSegments = setOf(
    "native", "package", "class", "object", "interface", "fun", "val", "var",
    "public", "private", "protected", "internal", "static", "final", "abstract",
    "import", "return", "new", "this", "super", "null", "true", "false", "is",
    "in", "if", "else", "for", "while", "do", "when", "try", "catch", "throw",
)

/**
 * Derives the Android namespace from the Gradle path, so `:feature:capture`
 * becomes `uk.co.jacoblevy.parallax.feature.capture`. One less thing to keep in
 * sync by hand, and it guarantees R-class uniqueness across modules.
 *
 * Path segments that are reserved words get a `_` suffix, because AGP (rightly)
 * rejects namespaces such as `...engine.native` outright.
 */
internal fun Project.defaultNamespace(): String {
    val suffix = path.removePrefix(":")
        .split(':')
        .map { segment ->
            val sanitized = segment.replace('-', '_')
            if (sanitized in ReservedPackageSegments) "${sanitized}_" else sanitized
        }
        .joinToString(".")
    return "uk.co.jacoblevy.parallax.$suffix"
}
