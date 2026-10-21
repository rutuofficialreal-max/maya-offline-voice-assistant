package com.maya.assistant

import android.os.Handler
import android.os.Looper

data class DownloadStatus(
    val active: Boolean = false,
    val fraction: Float = 0f,
    val title: String = "",
    val detail: String = "",
    val error: String? = null,
    val paused: Boolean = false,
)

/** In-process state shared between the services and the activity (all in one process). */
object MayaBus {
    private val main = Handler(Looper.getMainLooper())

    /** Called on the main thread whenever anything below changes. */
    @Volatile var listener: (() -> Unit)? = null

    @Volatile var running = false          // MayaService is up (loading or listening)
    @Volatile var ready = false            // engine loaded and listening
    @Volatile var state = NativeBridge.STATE_STOPPED
    @Volatile var statusLine = ""
    @Volatile var backendLine = ""
    @Volatile var lastHeard = ""
    @Volatile var lastReply = ""
    @Volatile var download = DownloadStatus()
    @Volatile var calibration = ""

    private val lines = ArrayDeque<String>()

    fun changed() {
        main.post { listener?.invoke() }
    }

    fun log(line: String) {
        synchronized(lines) {
            lines.addLast(line)
            while (lines.size > 120) lines.removeFirst()
        }
        changed()
    }

    fun logText(max: Int = 40): String = synchronized(lines) { lines.toList().takeLast(max).joinToString("\n") }

    fun beginExchange(heard: String) {
        lastHeard = heard
        lastReply = ""
        changed()
    }

    fun appendReply(delta: String) {
        lastReply += delta
        changed()
    }
}
