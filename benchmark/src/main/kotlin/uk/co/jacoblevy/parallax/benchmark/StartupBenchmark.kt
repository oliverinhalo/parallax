package uk.co.jacoblevy.parallax.benchmark

import androidx.benchmark.macro.BaselineProfileMode
import androidx.benchmark.macro.CompilationMode
import androidx.benchmark.macro.StartupMode
import androidx.benchmark.macro.StartupTimingMetric
import androidx.benchmark.macro.junit4.MacrobenchmarkRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * How long the app takes to become usable.
 *
 * Measured three ways because they fail differently. Cold startup is what a
 * user sees after a reboot or when the app has been evicted, and is dominated
 * by class loading and the first composition. Warm startup is the common case
 * and shows up regressions in the library query. Partial compilation is the
 * state a freshly installed app is actually in before ART has profiled it,
 * which is where a missing baseline profile shows.
 */
@RunWith(AndroidJUnit4::class)
class StartupBenchmark {

    @get:Rule
    val rule = MacrobenchmarkRule()

    @Test
    fun coldStartupFullyCompiled() = measure(StartupMode.COLD, CompilationMode.Full())

    @Test
    fun coldStartupAsInstalled() = measure(
        StartupMode.COLD,
        CompilationMode.Partial(baselineProfileMode = BaselineProfileMode.Disable),
    )

    @Test
    fun warmStartup() = measure(StartupMode.WARM, CompilationMode.Full())

    private fun measure(startup: StartupMode, compilation: CompilationMode) = rule.measureRepeated(
        packageName = PACKAGE,
        metrics = listOf(StartupTimingMetric()),
        compilationMode = compilation,
        iterations = ITERATIONS,
        startupMode = startup,
    ) {
        pressHome()
        startActivityAndWait()
    }

    private companion object {
        const val PACKAGE = "uk.co.jacoblevy.parallax"

        /**
         * Ten runs. Startup variance on a phone is a few percent, and ten is
         * where the median stops moving between runs of the suite itself.
         */
        const val ITERATIONS = 10
    }
}
