package uk.co.jacoblevy.parallax.feature.viewer.render

import com.google.common.truth.Truth.assertThat
import kotlinx.serialization.json.Json
import org.junit.Test

/**
 * The serialiser exists.
 *
 * Not a test of the parsing, which is obvious, but of the build: `@Serializable` without the
 * compiler plugin applied to this module compiles cleanly and throws at the point of use, and
 * the point of use here is behind a JNI call that cannot run in CI.
 */
class PreviewResultTest {

    @Test
    fun `the native preview answer has a generated serialiser`() {
        val decoded = Json { ignoreUnknownKeys = true }.decodeFromString<PreviewResult>(
            """{"success":true,"vertexCount":12,"triangleCount":4,"unexpected":"ignored"}""",
        )

        assertThat(decoded.success).isTrue()
        assertThat(decoded.vertexCount).isEqualTo(12)
        assertThat(decoded.triangleCount).isEqualTo(4)
    }
}
