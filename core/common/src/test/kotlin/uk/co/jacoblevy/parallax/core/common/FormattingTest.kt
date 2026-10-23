package uk.co.jacoblevy.parallax.core.common

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.common.util.formatArea
import uk.co.jacoblevy.parallax.core.common.util.formatBytes
import uk.co.jacoblevy.parallax.core.common.util.formatCount
import uk.co.jacoblevy.parallax.core.common.util.formatDuration
import uk.co.jacoblevy.parallax.core.common.util.formatEta
import uk.co.jacoblevy.parallax.core.common.util.formatLength
import uk.co.jacoblevy.parallax.core.common.util.formatPercent

class FormattingTest {

    @Test
    fun `bytes use binary prefixes and shrink the decimal as the number grows`() {
        assertThat(formatBytes(0)).isEqualTo("0 B")
        assertThat(formatBytes(1_023)).isEqualTo("1023 B")
        assertThat(formatBytes(1_024)).isEqualTo("1.0 KB")
        assertThat(formatBytes(1_536)).isEqualTo("1.5 KB")
        // Past ten units the decimal is dropped, which keeps the column width stable.
        assertThat(formatBytes(11L * 1024)).isEqualTo("11 KB")
        assertThat(formatBytes(5L * 1024 * 1024 * 1024)).isEqualTo("5.0 GB")
    }

    @Test
    fun `a negative byte count is an em dash rather than a negative size`() {
        assertThat(formatBytes(-1)).isEqualTo("—")
        assertThat(formatDuration(-1)).isEqualTo("—")
    }

    @Test
    fun `the largest prefix is used rather than overflowing past terabytes`() {
        val petabyte = 1024L * 1024 * 1024 * 1024 * 1024
        assertThat(formatBytes(petabyte)).endsWith("TB")
    }

    @Test
    fun `durations round rather than truncate`() {
        // Truncating would show "0s" on a task that still has half a second to run.
        assertThat(formatDuration(600)).isEqualTo("1s")
        assertThat(formatDuration(59_400)).isEqualTo("59s")
        assertThat(formatDuration(60_000)).isEqualTo("1m")
        assertThat(formatDuration(90_000)).isEqualTo("1m 30s")
        assertThat(formatDuration(3_600_000)).isEqualTo("1h")
        assertThat(formatDuration(3_900_000)).isEqualTo("1h 5m")
    }

    @Test
    fun `a whole number of minutes or hours omits the smaller unit`() {
        assertThat(formatDuration(120_000)).isEqualTo("2m")
        assertThat(formatDuration(7_200_000)).isEqualTo("2h")
    }

    @Test
    fun `an unknown eta says so instead of guessing`() {
        assertThat(formatEta(null)).isEqualTo("Estimating…")
        assertThat(formatEta(1_000)).isEqualTo("Almost done")
        assertThat(formatEta(90_000)).isEqualTo("1m 30s left")
    }

    @Test
    fun `a progress value slightly out of range cannot show over one hundred percent`() {
        assertThat(formatPercent(1.004f)).isEqualTo("100%")
        assertThat(formatPercent(-0.01f)).isEqualTo("0%")
        assertThat(formatPercent(0.5f)).isEqualTo("50%")
    }

    @Test
    fun `counts abbreviate at a thousand and a million`() {
        assertThat(formatCount(999)).isEqualTo("999")
        assertThat(formatCount(1_500)).isEqualTo("1.5K")
        assertThat(formatCount(2_400_000)).isEqualTo("2.4M")
    }

    @Test
    fun `metric lengths pick the unit that reads best`() {
        assertThat(formatLength(0.004, imperial = false)).isEqualTo("4.0 mm")
        assertThat(formatLength(0.42, imperial = false)).isEqualTo("42.0 cm")
        assertThat(formatLength(2.4384, imperial = false)).isEqualTo("2.438 m")
    }

    @Test
    fun `imperial lengths switch from inches to feet and inches at a foot`() {
        assertThat(formatLength(0.1, imperial = true)).isEqualTo("3.9 in")
        // 2.4384 m is exactly eight feet.
        assertThat(formatLength(2.4384, imperial = true)).isEqualTo("8′ 0.0″")
    }

    @Test
    fun `areas convert to square feet`() {
        assertThat(formatArea(13.02, imperial = false)).isEqualTo("13.02 m²")
        assertThat(formatArea(1.0, imperial = true)).isEqualTo("10.8 ft²")
    }
}
