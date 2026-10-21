package com.maya.assistant

import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.io.IOException
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Foreground service that downloads Maya's models after install (they are not part of the APK).
 * Shows a progress notification, resumes interrupted files (HTTP Range), waits for Wi-Fi if asked to,
 * verifies checksums and unpacks archives.
 */
class ModelDownloadService : Service() {
    companion object {
        const val ACTION_START = "com.maya.assistant.action.DOWNLOAD_START"
        const val ACTION_PAUSE = "com.maya.assistant.action.DOWNLOAD_PAUSE"
        private const val NOTIF_ID = 11
        private const val DONE_NOTIF_ID = 12
        private const val TAG = "MayaDownload"

        fun start(ctx: Context) {
            ctx.startForegroundService(Intent(ctx, ModelDownloadService::class.java).setAction(ACTION_START))
        }

        fun pause(ctx: Context) {
            ctx.startService(Intent(ctx, ModelDownloadService::class.java).setAction(ACTION_PAUSE))
        }
    }

    private val cancelled = AtomicBoolean(false)
    @Volatile private var worker: Thread? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var lastNotify = 0L

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Notifications.ensureChannels(this)
        if (intent?.action == ACTION_PAUSE) {
            cancelled.set(true)
            return START_NOT_STICKY
        }
        // Every startForegroundService() must be answered with startForeground(), even if a worker already runs.
        startForeground(
            NOTIF_ID,
            Notifications.download(this, "Downloading Maya's models", "Preparing…", 0, true),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
        )
        if (worker?.isAlive == true) return START_NOT_STICKY
        cancelled.set(false)
        worker = Thread({ runDownloads() }, "maya-download").also { it.start() }
        return START_NOT_STICKY
    }

    /** Android 15 caps dataSync foreground services; pause cleanly (progress is kept, the user can resume). */
    override fun onTimeout(startId: Int, fgsType: Int) {
        cancelled.set(true)
    }

    override fun onDestroy() {
        cancelled.set(true)
        releaseWakeLock()
        super.onDestroy()
    }

    // ------------------------------------------------------------------------------------------

    private fun runDownloads() {
        val prefs = Prefs(this)
        var error: String? = null
        var paused = false
        acquireWakeLock()
        try {
            val todo = ModelManager.missing(this)
            if (todo.isEmpty()) {
                finish(true, "Everything is installed. Maya works fully offline now.", false)
                return
            }
            val needed = todo.sumOf { it.bytes }
            val margin = 250L * 1024 * 1024
            val free = filesDir.usableSpace
            if (free < needed + margin) {
                throw IOException("Not enough free storage: about ${mb(needed + margin)} MB needed, ${mb(free)} MB available.")
            }
            ModelManager.rootDir(this).mkdirs()
            var finished = 0L
            for ((i, item) in todo.withIndex()) {
                waitForNetwork(prefs)
                install(item, i, todo.size, finished, needed, prefs)
                finished += item.bytes
            }
        } catch (e: DownloadCancelled) {
            paused = true
        } catch (e: Exception) {
            Log.e(TAG, "download failed", e)
            error = e.message ?: e.javaClass.simpleName
        } finally {
            releaseWakeLock()
        }
        when {
            error != null -> finish(false, error, false)
            paused -> finish(false, "Paused. Tap Resume to continue where it stopped.", true)
            else -> finish(true, "Everything is installed. Maya works fully offline now.", false)
        }
    }

    private fun install(item: ModelManager.Item, index: Int, count: Int, doneBefore: Long, totalAll: Long, prefs: Prefs) {
        val isCancelled = { cancelled.get() }
        val label = "${index + 1}/$count \u00B7 ${item.title}"
        fun fraction(done: Long): Float = ((doneBefore + done).toDouble() / totalAll).coerceIn(0.0, 0.995).toFloat()

        val partDir = File(ModelManager.rootDir(this), ".downloads").apply { mkdirs() }
        val part = File(partDir, item.id + ".part")

        var attempt = 0
        while (true) {
            try {
                Downloader.fetch(item.url, part, isCancelled) { done, total ->
                    val size = if (total > 0) total else item.bytes
                    publish(label, "${mb(done)} / ${mb(size)} MB", fraction(done))
                }
                break
            } catch (e: DownloadCancelled) {
                throw e
            } catch (e: IOException) {
                attempt++
                if (attempt >= 6) throw IOException("Network problem while downloading ${item.title}: ${e.message}")
                val wait = 2 shl attempt  // 4, 8, 16, 32, 64 s
                publish(label, "Connection lost, retrying in ${wait}s\u2026", fraction(part.length()))
                sleepCancellable(wait)
                waitForNetwork(prefs)
            }
        }

        item.sha256?.let { expected ->
            publish(label, "Verifying download\u2026", fraction(item.bytes))
            val actual = Downloader.sha256(part, isCancelled) { }
            if (!actual.equals(expected, ignoreCase = true)) {
                part.delete()
                throw IOException("Checksum mismatch for ${item.title}. The file was discarded; tap Resume to download it again.")
            }
        }

        val dir = ModelManager.itemDir(this, item)
        when (item.kind) {
            ModelManager.Kind.FILE -> {
                dir.mkdirs()
                val dest = File(dir, item.fileName)
                dest.delete()
                if (!part.renameTo(dest)) throw IOException("Could not move ${item.fileName} into place")
            }
            ModelManager.Kind.TAR_BZ2 -> {
                publish(label, "Unpacking\u2026", fraction(item.bytes))
                Downloader.extractTarBz2(part, dir, isCancelled)
                part.delete()
            }
        }
    }

    // ------------------------------------------------------------------------------------------

    private fun publish(title: String, detail: String, fraction: Float) {
        MayaBus.download = DownloadStatus(active = true, fraction = fraction, title = title, detail = detail)
        MayaBus.changed()
        val now = SystemClock.elapsedRealtime()
        if (now - lastNotify > 900) {
            lastNotify = now
            getSystemService(android.app.NotificationManager::class.java).notify(
                NOTIF_ID,
                Notifications.download(this, "Downloading Maya's models", "$title \u00B7 $detail", (fraction * 100).toInt(), false)
            )
        }
    }

    private fun finish(ok: Boolean, text: String, paused: Boolean) {
        MayaBus.download = DownloadStatus(
            active = false,
            fraction = if (ok) 1f else MayaBus.download.fraction,
            title = if (ok) "Models installed" else "Download stopped",
            detail = text,
            error = if (!ok && !paused) text else null,
            paused = paused
        )
        MayaBus.changed()
        Handler(Looper.getMainLooper()).post {
            stopForeground(STOP_FOREGROUND_REMOVE)
            getSystemService(android.app.NotificationManager::class.java)
                .notify(DONE_NOTIF_ID, Notifications.downloadFinished(this, ok, text))
            stopSelf()
        }
    }

    private fun waitForNetwork(prefs: Prefs) {
        var waited = 0
        while (!cancelled.get()) {
            if (networkOk(prefs.wifiOnly)) return
            val msg = if (prefs.wifiOnly) "Waiting for Wi-Fi\u2026" else "Waiting for a network connection\u2026"
            publish("Paused", msg, MayaBus.download.fraction)
            sleepCancellable(3)
            waited += 3
            if (waited > 45 * 60) throw IOException("No usable network connection for 45 minutes.")
        }
        throw DownloadCancelled()
    }

    private fun networkOk(wifiOnly: Boolean): Boolean {
        val cm = getSystemService(ConnectivityManager::class.java)
        val caps = cm.getNetworkCapabilities(cm.activeNetwork) ?: return false
        if (!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)) return false
        return !wifiOnly || caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)
    }

    private fun sleepCancellable(seconds: Int) {
        var left = seconds * 4
        while (left-- > 0) {
            if (cancelled.get()) throw DownloadCancelled()
            Thread.sleep(250)
        }
    }

    private fun acquireWakeLock() {
        val pm = getSystemService(PowerManager::class.java)
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Maya:download").apply {
            setReferenceCounted(false)
            acquire(3L * 60 * 60 * 1000)
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }

    private fun mb(bytes: Long): Long = bytes / (1024 * 1024)
}
