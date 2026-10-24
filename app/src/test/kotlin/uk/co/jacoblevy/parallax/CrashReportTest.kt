package uk.co.jacoblevy.parallax

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.diagnostics.CrashReport

class CrashReportTest {

    private val device = CrashReport.DeviceFacts(
        manufacturer = "samsung",
        model = "SM-S948B",
        release = "17",
        sdkInt = 37,
        abis = listOf("arm64-v8a"),
    )

    private fun format(throwable: Throwable) = CrashReport.format(
        throwable = throwable,
        threadName = "main",
        versionName = "1.0.0",
        versionCode = 1,
        device = device,
        occurredAtMillis = 1_761_000_000_000,
    )

    @Test
    fun `a report names the device, the build and the moment it happened`() {
        val report = format(IllegalStateException("no"))

        assertThat(report).contains("samsung SM-S948B")
        assertThat(report).contains("Android:  17 (API 37)")
        assertThat(report).contains("1.0.0 (1)")
        assertThat(report).contains("2025-10-20T22:40:00Z")
    }

    @Test
    fun `a report carries the whole cause chain, not just the exception that surfaced`() {
        // The top frame of a dependency-injection failure is always the injector. Everything
        // that says which dependency, and why, is further down the chain.
        val root = UnsatisfiedLinkError("libparallax_engine.so not found")
        val middle = RuntimeException("Cannot create an instance of ProjectRepository", root)
        val surface = IllegalStateException("Failed to build the component", middle)

        val report = format(surface)

        assertThat(report).contains("Failed to build the component")
        assertThat(report).contains("Caused by: java.lang.RuntimeException")
        assertThat(report).contains("libparallax_engine.so not found")
    }

    @Test
    fun `the headline is the root cause rather than the wrapper`() {
        val root = NoSuchMethodError("No static method getInstance()")
        val wrapper = RuntimeException("Unable to create application", root)

        assertThat(CrashReport.headline(wrapper))
            .isEqualTo("java.lang.NoSuchMethodError: No static method getInstance()")
    }

    @Test
    fun `an exception with no message still produces a usable headline`() {
        assertThat(CrashReport.headline(NullPointerException()))
            .isEqualTo("java.lang.NullPointerException")
    }
}
