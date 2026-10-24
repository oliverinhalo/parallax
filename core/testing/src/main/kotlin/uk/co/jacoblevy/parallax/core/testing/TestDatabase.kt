package uk.co.jacoblevy.parallax.core.testing

import java.util.concurrent.Executor

/**
 * Runs work on the calling thread.
 *
 * Room is given this as its query and transaction executor in tests so that a write and the
 * `Flow` emission it triggers both complete before the next line of the test runs. With Room's
 * default executors those emissions arrive on a real background thread, which under a virtual
 * clock may not have happened by the time the assertion reads the state -- the classic
 * once-in-twenty-runs failure.
 */
val DirectExecutor: Executor = Executor { command -> command.run() }
