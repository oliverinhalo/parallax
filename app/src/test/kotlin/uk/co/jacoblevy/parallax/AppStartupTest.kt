package uk.co.jacoblevy.parallax

import android.app.Application
import androidx.test.core.app.ApplicationProvider
import com.google.common.truth.Truth.assertThat
import kotlinx.serialization.serializer
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import uk.co.jacoblevy.parallax.diagnostics.CrashReporter
import uk.co.jacoblevy.parallax.navigation.ParallaxRoute

/**
 * Launching the app.
 *
 * Everything below `MainActivity` had tests of its own, and every one of them passed while the
 * app died on its first frame: nothing had ever built the whole graph and composed the
 * navigation host on top of it. A missing compiler plugin, a dependency that cannot be
 * constructed, a theme the splash screen cannot extend — none of those are visible to a test
 * that starts at a ViewModel.
 */
@RunWith(RobolectricTestRunner::class)
class AppStartupTest {

    @Test
    fun `the application builds its dependency graph`() {
        val application = ApplicationProvider.getApplicationContext<Application>()

        assertThat(application).isInstanceOf(ParallaxApplication::class.java)
    }

    @Test
    fun `the work manager configuration is supplied by the application`() {
        // WorkManager's own initialiser is removed in the manifest, so without this the
        // reconstruction worker would be built reflectively, without its six dependencies,
        // and fail the first time a scan was queued.
        val application = ApplicationProvider.getApplicationContext<ParallaxApplication>()

        assertThat(application.workManagerConfiguration.workerFactory).isNotNull()
    }

    @Test
    fun `nothing in the app's own code throws on the way to the first frame`() {
        val controller = Robolectric.buildActivity(MainActivity::class.java)

        val failure = runCatching { controller.setup() }.exceptionOrNull()

        // Only frames in our own packages count. ARCore's availability check binds Play's
        // install service, and Robolectric answers a bind it knows nothing about by calling
        // back with a null binder, which ARCore dereferences -- a failure of the emulation,
        // with no Parallax frame in it, where a device either binds or fails cleanly.
        assertThat(ourFramesIn(failure)).isEmpty()
    }

    @Test
    fun `every navigation route has a generated serialiser`() {
        // Type-safe navigation looks these up reflectively as the graph is built, so a module
        // without the serialization compiler plugin compiles and then throws on the first
        // composition. Asserting it here names the route rather than leaving a stack trace
        // from inside Navigation to be read backwards.
        val routes = listOf(
            serializer<ParallaxRoute.Onboarding>(),
            serializer<ParallaxRoute.Library>(),
            serializer<ParallaxRoute.Capture>(),
            serializer<ParallaxRoute.Processing>(),
            serializer<ParallaxRoute.Viewer>(),
            serializer<ParallaxRoute.Export>(),
            serializer<ParallaxRoute.ArPlacement>(),
            serializer<ParallaxRoute.Settings>(),
            serializer<ParallaxRoute.Licences>(),
        )

        assertThat(routes.map { it.descriptor.serialName }).containsNoDuplicates()
    }

    @Test
    fun `with no stored crash report there is nothing to show`() {
        val application = ApplicationProvider.getApplicationContext<Application>()
        CrashReporter.clear(application)

        assertThat(CrashReporter.showStoredReport(application)).isFalse()
    }

    @Test
    fun `a stored report is consumed by the attempt to show it, not by being seen`() {
        // Otherwise a screen that fails to appear leaves the report in place, and every
        // launch afterwards stops to show a crash screen that never arrives -- which is
        // an app that cannot be opened again until it is reinstalled.
        val application = ApplicationProvider.getApplicationContext<Application>()
        CrashReporter.store(application, "Parallax crash report\n\nboom")

        CrashReporter.showStoredReport(application)

        assertThat(CrashReporter.lastReport(application)).isNull()
    }

    private fun ourFramesIn(failure: Throwable?): List<String> =
        failure?.stackTraceToString()
            .orEmpty()
            .lines()
            .filter { it.contains("uk.co.jacoblevy.parallax") }
            // This test method drove the startup, so its own frame is in every trace and says
            // nothing about where the failure came from.
            .filterNot { it.contains(AppStartupTest::class.java.name) }
}
