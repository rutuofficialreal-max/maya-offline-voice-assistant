package com.maya.assistant

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.drawable.Icon
import android.os.Build

/** Channels and builders for every notification Maya shows. Framework APIs only. */
object Notifications {
    const val CH_STATUS = "maya_status"
    const val CH_LISTEN = "maya_listening"
    const val CH_DOWNLOAD = "maya_download"
    const val CH_DONE = "maya_done"

    fun ensureChannels(ctx: Context) {
        val nm = ctx.getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CH_STATUS, "Maya status", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Shown while Maya listens in the background"
                setShowBadge(false)
            })
        nm.createNotificationChannel(
            NotificationChannel(CH_LISTEN, "Maya is listening", NotificationManager.IMPORTANCE_HIGH).apply {
                description = "A silent pop-up the moment you say \u201CHey Maya\u201D"
                setSound(null, null)
                enableVibration(false)
                setShowBadge(false)
            })
        nm.createNotificationChannel(
            NotificationChannel(CH_DOWNLOAD, "Model download", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Progress while Maya's models download"
                setShowBadge(false)
            })
        nm.createNotificationChannel(
            NotificationChannel(CH_DONE, "Model download result", NotificationManager.IMPORTANCE_DEFAULT).apply {
                description = "Tells you when the download finished or stopped"
            })
    }

    private fun contentIntent(ctx: Context): PendingIntent = PendingIntent.getActivity(
        ctx, 0,
        Intent(ctx, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_NEW_TASK),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
    )

    private fun serviceAction(ctx: Context, cls: Class<*>, action: String, title: String, req: Int): Notification.Action {
        val pi = PendingIntent.getService(
            ctx, req, Intent(ctx, cls).setAction(action),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        return Notification.Action.Builder(Icon.createWithResource(ctx, R.drawable.ic_stat_maya), title, pi).build()
    }

    /** The ongoing foreground-service notification of the assistant. */
    fun status(ctx: Context, text: String, state: Int): Notification {
        val b = Notification.Builder(ctx, CH_STATUS)
            .setSmallIcon(R.drawable.ic_stat_maya)
            .setContentTitle("Maya")
            .setContentText(text)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setContentIntent(contentIntent(ctx))
        if (state == NativeBridge.STATE_THINKING || state == NativeBridge.STATE_SPEAKING) {
            b.addAction(serviceAction(ctx, MayaService::class.java, MayaService.ACTION_INTERRUPT, "Silence", 2))
        }
        b.addAction(serviceAction(ctx, MayaService::class.java, MayaService.ACTION_STOP, "Stop Maya", 1))
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        return b.build()
    }

    /** "Maya is listening..." pop-up, posted the instant the wake word fires. */
    fun listening(ctx: Context): Notification =
        Notification.Builder(ctx, CH_LISTEN)
            .setSmallIcon(R.drawable.ic_stat_maya)
            .setContentTitle("Maya is listening...")
            .setContentText("Go ahead, I'm all ears.")
            .setCategory(Notification.CATEGORY_STATUS)
            .setOnlyAlertOnce(true)
            .setAutoCancel(true)
            .setTimeoutAfter(12_000)
            .setContentIntent(contentIntent(ctx))
            .build()

    fun download(ctx: Context, title: String, text: String, percent: Int, indeterminate: Boolean): Notification {
        val b = Notification.Builder(ctx, CH_DOWNLOAD)
            .setSmallIcon(R.drawable.ic_stat_maya)
            .setContentTitle(title)
            .setContentText(text)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_PROGRESS)
            .setProgress(100, percent.coerceIn(0, 100), indeterminate)
            .setContentIntent(contentIntent(ctx))
            .addAction(serviceAction(ctx, ModelDownloadService::class.java, ModelDownloadService.ACTION_PAUSE, "Pause", 3))
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        return b.build()
    }

    fun downloadFinished(ctx: Context, ok: Boolean, text: String): Notification {
        val b = Notification.Builder(ctx, CH_DONE)
            .setSmallIcon(R.drawable.ic_stat_maya)
            .setContentTitle(if (ok) "Maya is ready" else "Download stopped")
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setAutoCancel(true)
            .setContentIntent(contentIntent(ctx))
        if (!ok) b.addAction(serviceAction(ctx, ModelDownloadService::class.java, ModelDownloadService.ACTION_START, "Resume", 4))
        return b.build()
    }
}
