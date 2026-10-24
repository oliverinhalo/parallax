package uk.co.jacoblevy.parallax.feature.processing

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import androidx.work.ForegroundInfo
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The reconstruction notifications.
 *
 * Two channels, because they are two different things to the user: progress is a silent ongoing
 * notification they may want to collapse, and a finished scan is a one-off they probably want
 * to hear. Lumping them together means either the progress bar buzzes every few seconds or the
 * finished scan arrives silently.
 */
@Singleton
class ProcessingNotifications @Inject constructor(
    @ApplicationContext private val context: Context,
) {
    private val manager = NotificationManagerCompat.from(context)

    init {
        createChannels()
    }

    /** The foreground notification WorkManager promotes the worker with. */
    fun foregroundInfo(title: String, detail: String?, progress: Float): ForegroundInfo {
        val notification = progressNotification(title, detail, progress)
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            // Android 14 requires the service type to be declared at promotion time, and
            // DATA_SYNC is the only type that fits a long local computation.
            ForegroundInfo(
                PROGRESS_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC,
            )
        } else {
            ForegroundInfo(PROGRESS_ID, notification)
        }
    }

    fun update(title: String, detail: String?, progress: Float) {
        post(PROGRESS_ID) { progressNotification(title, detail, progress) }
    }

    fun clearProgress() {
        runCatching { manager.cancel(PROGRESS_ID) }
    }

    fun notifyFinished(projectName: String, projectId: String) {
        // The id is derived from the project so that reprocessing the same scan replaces its
        // notification rather than stacking a second one.
        post(DONE_ID_BASE + projectId.hashCode()) {
            NotificationCompat.Builder(context, CHANNEL_DONE)
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setContentTitle("$projectName is ready")
                .setContentText("Tap to open the model.")
                .setAutoCancel(true)
                .setCategory(NotificationCompat.CATEGORY_STATUS)
                .build()
        }
    }

    fun notifyFailed(projectName: String, reason: String) {
        post(DONE_ID_BASE + projectName.hashCode()) {
            NotificationCompat.Builder(context, CHANNEL_DONE)
                .setSmallIcon(android.R.drawable.stat_notify_error)
                .setContentTitle("$projectName could not be processed")
                .setContentText(reason)
                // The reason is often two sentences, and a truncated explanation of a failure
                // is worse than none.
                .setStyle(NotificationCompat.BigTextStyle().bigText(reason))
                .setAutoCancel(true)
                .build()
        }
    }

    /**
     * Post a notification, if notifications are permitted at all.
     *
     * The permission is checked in [canPost], which Lint cannot follow through a helper, hence
     * the suppression. The `runCatching` is not redundant with the check either: the permission
     * can be revoked between the two, and a revoked notification permission must not take a
     * running reconstruction down with it.
     */
    @SuppressLint("MissingPermission")
    private inline fun post(id: Int, build: () -> Notification) {
        if (!canPost()) return
        runCatching { manager.notify(id, build()) }
    }

    private fun progressNotification(
        title: String,
        detail: String?,
        progress: Float,
    ): Notification = NotificationCompat.Builder(context, CHANNEL_PROGRESS)
        .setSmallIcon(android.R.drawable.stat_sys_download)
        .setContentTitle(title)
        .setContentText(detail ?: "Processing")
        .setProgress(PROGRESS_MAX, (progress * PROGRESS_MAX).toInt().coerceIn(0, PROGRESS_MAX), false)
        .setOngoing(true)
        .setOnlyAlertOnce(true)
        .setSilent(true)
        .setCategory(NotificationCompat.CATEGORY_PROGRESS)
        .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
        .build()

    /**
     * Whether notifications can be posted at all.
     *
     * From Android 13 this needs a runtime permission the user may have refused. Processing
     * still runs; it just runs without a progress notification, which is why every call here
     * checks rather than the app demanding the permission up front.
     */
    private fun canPost(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED

    private fun createChannels() {
        val progress = NotificationChannel(
            CHANNEL_PROGRESS,
            "Processing",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "Progress while a scan is being reconstructed."
            setShowBadge(false)
        }
        val done = NotificationChannel(
            CHANNEL_DONE,
            "Finished scans",
            NotificationManager.IMPORTANCE_DEFAULT,
        ).apply {
            description = "When a scan finishes processing, or cannot be processed."
        }
        runCatching { manager.createNotificationChannels(listOf(progress, done)) }
    }

    private companion object {
        const val CHANNEL_PROGRESS = "parallax.processing"
        const val CHANNEL_DONE = "parallax.processing.finished"
        const val PROGRESS_ID = 1001
        const val DONE_ID_BASE = 2000
        const val PROGRESS_MAX = 1000
    }
}
