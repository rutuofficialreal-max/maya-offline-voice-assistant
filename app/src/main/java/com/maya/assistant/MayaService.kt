package com.maya.assistant

import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * Microphone foreground service that hosts the native voice loop:
 * wake word -> Silero VAD -> Whisper -> Vulkan LLM -> streaming TTS -> back to passive listening.
 */
class MayaService : Service(), MayaListener {
    companion object {
        const val ACTION_START = "com.maya.assistant.action.START"
        const val ACTION_STOP = "com.maya.assistant.action.STOP"
        const val ACTION_INTERRUPT = "com.maya.assistant.action.INTERRUPT"
        const val ACTION_RELOAD_WAKE = "com.maya.assistant.action.RELOAD_WAKE"
        private const val NOTIF_ID = 1
        private const val LISTEN_NOTIF_ID = 2
        private const val TAG = "MayaService"

        fun start(ctx: Context) {
            ctx.startForegroundService(Intent(ctx, MayaService::class.java).setAction(ACTION_START))
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, MayaService::class.java).setAction(ACTION_STOP))
        }

        /** Applies the current sensitivity / calibrated phrases to a running engine without a restart. */
        fun reloadWake(ctx: Context) {
            ctx.startService(Intent(ctx, MayaService::class.java).setAction(ACTION_RELOAD_WAKE))
        }

        fun interrupt(ctx: Context) {
            ctx.startService(Intent(ctx, MayaService::class.java).setAction(ACTION_INTERRUPT))
        }
    }

    private val main = Handler(Looper.getMainLooper())
    private val exec = Executors.newSingleThreadExecutor()
    @Volatile private var handle = 0L
    @Volatile private var starting = false
    @Volatile private var stopping = false
    private var wakeLock: PowerManager.WakeLock? = null
    private var focus: AudioFocusRequest? = null
    private lateinit var nm: NotificationManager

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        nm = getSystemService(NotificationManager::class.java)
        Notifications.ensureChannels(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> shutdown()
            ACTION_INTERRUPT -> {
                if (handle != 0L) NativeBridge.nativeInterrupt(handle)
                stopIfIdle()
            }
            ACTION_RELOAD_WAKE -> {
                reloadWakeWord()
                stopIfIdle()
            }
            else -> startEngine()
        }
        return START_NOT_STICKY  // a microphone service cannot be restarted from the background on Android 14+
    }

    /** A helper intent (interrupt / reload) can create this service when nothing is running: do not leave it idle. */
    private fun stopIfIdle() {
        if (handle == 0L && !starting && !stopping) stopSelf()
    }

    private fun reloadWakeWord() {
        val h = handle
        if (h == 0L) return
        val prefs = Prefs(this)
        exec.execute {
            try {
                NativeBridge.nativeSetWake(h, prefs.activeThreshold, prefs.phrases.joinToString("\n"))
                NativeBridge.nativeSetWakeEngine(h, prefs.wakeEngine)
            } catch (t: Throwable) {
                Log.e(TAG, "nativeSetWake failed", t)
            }
        }
    }

    // ------------------------------------------------------------------------------------------

    private fun startEngine() {
        // startForegroundService() must always be answered with startForeground(). A microphone
        // service throws SecurityException when RECORD_AUDIO has not been granted.
        try {
            startForeground(
                NOTIF_ID,
                Notifications.status(this, "Starting Maya\u2026", NativeBridge.STATE_STOPPED),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            )
        } catch (e: SecurityException) {
            Log.e(TAG, "microphone foreground service refused", e)
            MayaBus.log("Android refused to start the microphone service. Grant the microphone permission and try again.")
            stopSelf()
            return
        }
        if (starting || stopping || handle != 0L) return
        starting = true
        acquireWakeLock()
        MayaBus.running = true
        MayaBus.ready = false
        MayaBus.state = NativeBridge.STATE_STOPPED
        MayaBus.statusLine = "Loading models\u2026"
        MayaBus.changed()

        exec.execute {
            val prefs = Prefs(this)
            // Crash guard: initInProgress survives a native crash. If it is still set, the previous
            // start died while loading (typically a Vulkan driver crash), so skip the GPU from now on.
            if (prefs.initInProgress) {
                prefs.gpuFailed = true
                MayaBus.log("The last start crashed while loading models; the GPU is disabled. Re-enable it in Settings to retry.")
            }
            val useGpu = prefs.useGpu && !prefs.gpuFailed

            prefs.initInProgress = true
            val h = try {
                NativeBridge.nativeCreate(ModelManager.buildConfig(this, prefs, useGpu), this)
            } catch (t: Throwable) {
                Log.e(TAG, "nativeCreate failed", t)
                MayaBus.log("Native error: ${t.message}")
                0L
            }
            prefs.initInProgress = false

            if (h == 0L || stopping) {
                if (h != 0L) {
                    NativeBridge.nativeDestroy(h)
                } else {
                    MayaBus.log("Maya could not start. Check that all models finished downloading.")
                }
                main.post { finishService() }
                return@execute
            }
            handle = h
            if (!NativeBridge.nativeStart(h)) {
                MayaBus.log("Could not open the microphone (is another app using it?).")
                shutdown()
                return@execute
            }
            MayaBus.ready = true
            MayaBus.statusLine = ""
            starting = false
            MayaBus.changed()
        }
    }

    private fun shutdown() {
        if (stopping) return
        stopping = true
        exec.execute {
            val h = handle
            handle = 0L
            if (h != 0L) {
                try {
                    NativeBridge.nativeStop(h)    // joins the native threads
                    NativeBridge.nativeDestroy(h) // frees every model and buffer
                } catch (t: Throwable) {
                    Log.e(TAG, "shutdown failed", t)
                }
            }
            main.post { finishService() }
        }
    }

    private fun finishService() {
        nm.cancel(LISTEN_NOTIF_ID)
        abandonFocus()
        releaseWakeLock()
        MayaBus.running = false
        MayaBus.ready = false
        MayaBus.state = NativeBridge.STATE_STOPPED
        MayaBus.statusLine = ""
        MayaBus.changed()
        starting = false
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        val h = handle
        handle = 0L
        if (h != 0L) {  // process teardown: make sure native memory and the microphone are released
            try {
                NativeBridge.nativeStop(h)
                NativeBridge.nativeDestroy(h)
            } catch (_: Throwable) {
            }
        }
        exec.shutdown()
        try { exec.awaitTermination(3, TimeUnit.SECONDS) } catch (_: InterruptedException) { }
        releaseWakeLock()
        abandonFocus()
        MayaBus.running = false
        MayaBus.ready = false
        super.onDestroy()
    }

    // ------------------------------------------------------------------------------------------
    // Events from the native engine (native threads)

    override fun onEvent(type: Int, payload: ByteArray?) {
        val text = if (payload == null) "" else String(payload, Charsets.UTF_8)
        when (type) {
            NativeBridge.EVT_STATE -> onState(text.toIntOrNull() ?: 0)
            NativeBridge.EVT_WAKE -> onWake()
            NativeBridge.EVT_TRANSCRIPT -> {
                MayaBus.beginExchange(text)
                MayaBus.log("You   \u203A $text")
            }
            NativeBridge.EVT_REPLY_DELTA -> MayaBus.appendReply(text)
            NativeBridge.EVT_REPLY_DONE -> MayaBus.log("Maya  \u203A ${MayaBus.lastReply.trim()}")
            NativeBridge.EVT_ERROR -> MayaBus.log("\u26A0 $text")
            NativeBridge.EVT_METRIC -> MayaBus.log("\u23F1 $text")
            NativeBridge.EVT_INFO -> {
                MayaBus.log(text)
                if (text.startsWith("LLM backend:")) MayaBus.backendLine = text.removePrefix("LLM backend: ")
                if (!MayaBus.ready) {
                    MayaBus.statusLine = text
                    nm.notify(NOTIF_ID, Notifications.status(this, text, NativeBridge.STATE_STOPPED))
                    MayaBus.changed()
                }
            }
        }
    }

    private fun onState(state: Int) {
        MayaBus.state = state
        if (state != NativeBridge.STATE_LISTENING) nm.cancel(LISTEN_NOTIF_ID)
        if (state == NativeBridge.STATE_THINKING || state == NativeBridge.STATE_SPEAKING) requestDuck() else abandonFocus()
        val line = when (state) {
            NativeBridge.STATE_PASSIVE -> "Say \u201CHey Maya\u201D"
            NativeBridge.STATE_LISTENING -> "Maya is listening\u2026"
            NativeBridge.STATE_TRANSCRIBING -> "Transcribing\u2026"
            NativeBridge.STATE_THINKING -> "Thinking\u2026"
            NativeBridge.STATE_SPEAKING -> "Speaking\u2026"
            NativeBridge.STATE_ERROR -> "Something went wrong. Open Maya for details."
            else -> null
        }
        if (line != null && !stopping) nm.notify(NOTIF_ID, Notifications.status(this, line, state))
        MayaBus.changed()
    }

    /** The wake word just fired: pop up "Maya is listening..." and give a tiny haptic tick. */
    private fun onWake() {
        nm.notify(LISTEN_NOTIF_ID, Notifications.listening(this))
        buzz()
        MayaBus.log("\u25CE Hey Maya")
    }

    private fun buzz() {
        try {
            val v: Vibrator = if (Build.VERSION.SDK_INT >= 31) {
                getSystemService(VibratorManager::class.java).defaultVibrator
            } else {
                @Suppress("DEPRECATION") getSystemService(Vibrator::class.java)
            }
            v.vibrate(VibrationEffect.createOneShot(35, VibrationEffect.DEFAULT_AMPLITUDE))
        } catch (_: Exception) {
        }
    }

    private fun requestDuck() {
        if (focus != null) return
        val am = getSystemService(AudioManager::class.java)
        val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_ASSISTANT)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build()
            )
            .setOnAudioFocusChangeListener { }
            .build()
        if (am.requestAudioFocus(req) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) focus = req
    }

    private fun abandonFocus() {
        val f = focus ?: return
        focus = null
        getSystemService(AudioManager::class.java).abandonAudioFocusRequest(f)
    }

    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        val pm = getSystemService(PowerManager::class.java)
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Maya:listening").apply {
            setReferenceCounted(false)
            acquire()  // released in finishService()/onDestroy()
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }
}
