package uk.co.jacoblevy.parallax.core.testing

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.TestCoroutineScheduler
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.rules.TestWatcher
import org.junit.runner.Description

/**
 * Replaces `Dispatchers.Main` with a test dispatcher that shares one scheduler.
 *
 * The shared [scheduler] is the point. A rule that creates its own dispatcher leaves the
 * ViewModel's `viewModelScope` on a different scheduler from the test body's, so
 * `advanceUntilIdle()` in the test does not advance the ViewModel's coroutines and anything
 * involving a delay -- a debounce, a retry backoff -- silently never runs. Passing [dispatcher]
 * to `runTest` keeps both on the same virtual clock.
 *
 * Unconfined rather than standard: a ViewModel that updates its state in `init` should have
 * done so by the time the test's first assertion runs, without the test having to know that it
 * needs a `runCurrent()` first.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class MainDispatcherRule : TestWatcher() {

    val scheduler = TestCoroutineScheduler()
    val dispatcher: CoroutineDispatcher = UnconfinedTestDispatcher(scheduler)

    override fun starting(description: Description) {
        Dispatchers.setMain(dispatcher)
    }

    override fun finished(description: Description) {
        Dispatchers.resetMain()
    }
}
