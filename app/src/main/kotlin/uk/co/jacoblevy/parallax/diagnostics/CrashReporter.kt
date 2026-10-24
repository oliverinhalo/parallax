package uk.co.jacoblevy.parallax.diagnostics

import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import java.io.File

/**
 * Catches what nothing else caught.
 *
 * Every failure a user is meant to see is a `ParallaxError` with a recovery. This is for the
 * ones nobody anticipated: it writes the report to the app's own storage and shows it on a
 * screen the user can share from.
 *
 * It runs in a second process. A crash during application startup leaves the main process
 * unable to start at all, so a screen hosted in that process would never appear -- which is
 * exactly the crash a user most needs to be able to report.
 */
object CrashReporter {

    private const val TAG = "ParallaxCrash"
    private const val FILE_NAME = "last-crash.txt"

    /**
     * Installs the handler. Call first in `attachBaseContext`: anything constructed before this
     * line is outside what the reporter can see, and application startup is where the crashes
     * a user cannot otherwise report happen.
     */
    fun install(context: Context) {
        val application = context.applicationContext
        val previous = Thread.getDefaultUncaughtExceptionHandler()

        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            // Anything thrown in here replaces the report with a second, less useful crash, so
            // every step is independently guarded and the original handler still runs.
            val report = runCatching { report(application, thread.name, throwable) }
                .getOrElse { "Parallax crash report\n\n${throwable.stackTraceToString()}" }

            Log.e(TAG, report)
            runCatching { store(application, report) }
            runCatching { show(application, report) }

            // Delegating is what ends the process: on Android the handler installed before
            // this one is the platform's, which logs to logcat and tears the process down the
            // way the system expects. Killing the process here instead would pre-empt that --
            // and would take the JVM down mid-suite under Robolectric, where there is no
            // platform handler and an uncaught exception is a test result rather than a death.
            previous?.uncaughtException(thread, throwable)
        }
    }

    /** The last report written, or null. The crash screen is shown instead where it can be. */
    fun lastReport(context: Context): String? =
        file(context).takeIf(File::exists)?.runCatching { readText() }?.getOrNull()

    /**
     * Shows an unread report from a previous run, if there is one, and says whether it did.
     *
     * The caller is expected to stop what it was doing when this returns true: the screen is
     * the whole point of the launch, and starting the app behind it would leave the user
     * looking at a crash report with a half-built app underneath.
     */
    fun showStoredReport(context: Context): Boolean {
        val report = lastReport(context) ?: return false
        // Cleared before it is shown, not after. If showing it fails -- the screen never
        // starts, or dies rendering -- a report cleared afterwards would still be there on the
        // next launch, and the app would stop at this line every time it was opened, with
        // nothing to show for it and no way out but reinstalling.
        clear(context)
        return runCatching { show(context, report) }.isSuccess
    }

    /** Forgets the stored report, once the user has seen it. */
    fun clear(context: Context) {
        runCatching { file(context).delete() }
    }

    private fun report(context: Context, threadName: String, throwable: Throwable): String {
        val packages = context.packageManager
        val info = packages.getPackageInfo(context.packageName, 0)
        @Suppress("DEPRECATION")
        val versionCode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            info.longVersionCode
        } else {
            info.versionCode.toLong()
        }

        return CrashReport.format(
            throwable = throwable,
            threadName = threadName,
            versionName = info.versionName.orEmpty(),
            versionCode = versionCode,
            device = CrashReport.DeviceFacts(
                manufacturer = Build.MANUFACTURER,
                model = Build.MODEL,
                release = Build.VERSION.RELEASE,
                sdkInt = Build.VERSION.SDK_INT,
                abis = Build.SUPPORTED_ABIS.toList(),
            ),
            occurredAtMillis = System.currentTimeMillis(),
        )
    }

    /** Stores a report as the handler would. Visible so the recovery path can be tested. */
    internal fun store(context: Context, report: String) {
        file(context).writeText(report)
    }

    private fun show(context: Context, report: String) {
        val intent = Intent(context, CrashActivity::class.java)
            .putExtra(CrashActivity.EXTRA_REPORT, report)
            // A new task because the crashing one is about to be torn down, and outside recents
            // so a stale crash screen cannot be resumed later as if it were the app.
            .addFlags(
                Intent.FLAG_ACTIVITY_NEW_TASK or
                    Intent.FLAG_ACTIVITY_CLEAR_TASK or
                    Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS,
            )
        context.startActivity(intent)
    }

    private fun file(context: Context) = File(context.filesDir, FILE_NAME)
}
