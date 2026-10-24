package uk.co.jacoblevy.parallax.benchmark

import androidx.benchmark.macro.CompilationMode
import androidx.benchmark.macro.FrameTimingMetric
import androidx.benchmark.macro.StartupMode
import androidx.benchmark.macro.junit4.MacrobenchmarkRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.uiautomator.By
import androidx.test.uiautomator.Direction
import androidx.test.uiautomator.Until
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Frame timing while scrolling the library.
 *
 * The library is the first thing anyone sees and the only screen in the app
 * that scrolls a long list of images, so it is where dropped frames would show
 * first. `FrameTimingMetric` reports the 50th, 90th, 95th and 99th percentile
 * frame durations; the 99th is the one that matters, because a single 40 ms
 * frame in a flick is what people perceive as jank.
 */
@RunWith(AndroidJUnit4::class)
class LibraryScrollBenchmark {

    @get:Rule
    val rule = MacrobenchmarkRule()

    @Test
    fun scrollLibrary() = rule.measureRepeated(
        packageName = PACKAGE,
        metrics = listOf(FrameTimingMetric()),
        compilationMode = CompilationMode.Full(),
        iterations = ITERATIONS,
        startupMode = StartupMode.WARM,
        setupBlock = {
            pressHome()
            startActivityAndWait()
        },
    ) {
        // Wait for the grid rather than a fixed sleep: on a slow device the
        // first frames would otherwise be measured against an empty screen.
        device.wait(Until.hasObject(By.scrollable(true)), GRID_TIMEOUT_MILLIS)
        val grid = device.findObject(By.scrollable(true)) ?: return@measureRepeated
        grid.setGestureMargin(device.displayWidth / GESTURE_MARGIN_FRACTION)
        repeat(SCROLLS) {
            grid.fling(Direction.DOWN)
            device.waitForIdle()
        }
        grid.fling(Direction.UP)
        device.waitForIdle()
    }

    private companion object {
        const val PACKAGE = "uk.co.jacoblevy.parallax"
        const val ITERATIONS = 8
        const val SCROLLS = 3
        const val GRID_TIMEOUT_MILLIS = 5_000L

        /**
         * Keep the fling away from the edges, where the system's back gesture
         * would swallow it and the benchmark would measure nothing.
         */
        const val GESTURE_MARGIN_FRACTION = 5
    }
}
