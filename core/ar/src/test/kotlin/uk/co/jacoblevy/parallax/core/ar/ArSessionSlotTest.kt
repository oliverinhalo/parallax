package uk.co.jacoblevy.parallax.core.ar

import com.google.common.truth.Truth.assertThat
import org.junit.After
import org.junit.Test

/**
 * The one-session rule.
 *
 * Worth a test of its own because the thing it protects against cannot be tested: a second
 * ARCore session kills the process in native code, so the rule has to be provably held here
 * rather than observed failing there.
 */
class ArSessionSlotTest {

    private val first = Any()
    private val second = Any()

    @After
    fun `leave the slot free`() {
        ArSessionSlot.release(first)
        ArSessionSlot.release(second)
    }

    @Test
    fun `the first claim succeeds and the second is refused`() {
        assertThat(ArSessionSlot.acquire(first)).isTrue()

        assertThat(ArSessionSlot.acquire(second)).isFalse()
    }

    @Test
    fun `releasing lets the next claim through`() {
        ArSessionSlot.acquire(first)
        ArSessionSlot.release(first)

        assertThat(ArSessionSlot.acquire(second)).isTrue()
    }

    @Test
    fun `a release from someone who does not hold it changes nothing`() {
        // The mode switch that caused this: a renderer being disposed released the slot the
        // renderer that replaced it was already holding, and the next create then made a
        // second session beside the live one.
        ArSessionSlot.acquire(first)

        ArSessionSlot.release(second)

        assertThat(ArSessionSlot.isHeldBy(first)).isTrue()
        assertThat(ArSessionSlot.acquire(second)).isFalse()
    }

    @Test
    fun `claiming twice from the same holder still leaves one session`() {
        assertThat(ArSessionSlot.acquire(first)).isTrue()

        // Not reentrant on purpose: the second call is a bug at the call site, and reporting it
        // as a refusal shows a message instead of quietly allowing two sessions.
        assertThat(ArSessionSlot.acquire(first)).isFalse()
        assertThat(ArSessionSlot.isHeldBy(first)).isTrue()
    }
}
