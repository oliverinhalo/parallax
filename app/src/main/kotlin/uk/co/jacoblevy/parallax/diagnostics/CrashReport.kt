package uk.co.jacoblevy.parallax.diagnostics

import java.io.PrintWriter
import java.io.StringWriter

/**
 * The text of a crash report.
 *
 * Separate from the handler that installs it and the screen that shows it, because this is the
 * part with a right and a wrong answer: a report missing the cause chain, the device or the
 * build is a report that cannot be acted on, and that is worth a test.
 *
 * Nothing here leaves the device. The report is written to the app's own storage and shown to
 * the user, who decides whether to share it and with whom.
 */
object CrashReport {

    /** Formats [throwable] together with the facts needed to place it. */
    fun format(
        throwable: Throwable,
        threadName: String,
        versionName: String,
        versionCode: Long,
        device: DeviceFacts,
        occurredAtMillis: Long,
    ): String = buildString {
        appendLine("Parallax crash report")
        appendLine("=====================")
        appendLine()
        appendLine("When:     ${isoUtc(occurredAtMillis)}")
        appendLine("Version:  $versionName ($versionCode)")
        appendLine("Device:   ${device.manufacturer} ${device.model}")
        appendLine("Android:  ${device.release} (API ${device.sdkInt})")
        appendLine("ABIs:     ${device.abis.joinToString()}")
        appendLine("Thread:   $threadName")
        appendLine()
        appendLine(stackTraceOf(throwable))
    }

    /** The first line of a report, for the screen's heading. */
    fun headline(throwable: Throwable): String {
        val root = generateSequence(throwable, Throwable::cause).last()
        val message = root.message?.takeIf(String::isNotBlank)
        return listOfNotNull(root.javaClass.name, message).joinToString(": ")
    }

    private fun stackTraceOf(throwable: Throwable): String {
        val writer = StringWriter()
        // printStackTrace walks the cause chain and the suppressed exceptions, which is the
        // whole point: the top frame of a Hilt or reflection failure is almost never the bug.
        PrintWriter(writer).use(throwable::printStackTrace)
        return writer.toString()
    }

    private fun isoUtc(millis: Long): String {
        val instant = java.time.Instant.ofEpochMilli(millis)
        return java.time.format.DateTimeFormatter.ISO_INSTANT.format(instant)
    }

    /** The device facts a report carries, passed in so the formatting can be tested. */
    data class DeviceFacts(
        val manufacturer: String,
        val model: String,
        val release: String,
        val sdkInt: Int,
        val abis: List<String>,
    )
}
