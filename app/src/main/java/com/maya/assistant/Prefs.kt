package com.maya.assistant

import android.content.Context
import kotlin.math.pow

/** Small SharedPreferences wrapper. Crash-sensitive flags use commit() so they survive a native crash. */
class Prefs(context: Context) {
    private val sp = context.applicationContext.getSharedPreferences("maya", Context.MODE_PRIVATE)

    var wifiOnly: Boolean
        get() = sp.getBoolean("wifi_only", true)
        set(v) { sp.edit().putBoolean("wifi_only", v).apply() }

    var useGpu: Boolean
        get() = sp.getBoolean("use_gpu", true)
        set(v) { sp.edit().putBoolean("use_gpu", v).apply() }

    /** 0..100, higher = triggers more easily (and more false triggers). */
    var sensitivity: Int
        get() = sp.getInt("sensitivity", 55)
        set(v) { sp.edit().putInt("sensitivity", v.coerceIn(0, 100)).apply() }

    /** Set while models are loading; still set at next start => the last start died natively. */
    var initInProgress: Boolean
        get() = sp.getBoolean("init_in_progress", false)
        set(v) { sp.edit().putBoolean("init_in_progress", v).commit() }

    /** Set after a crash during model loading: Vulkan is skipped until the user re-enables it. */
    var gpuFailed: Boolean
        get() = sp.getBoolean("gpu_failed", false)
        set(v) { sp.edit().putBoolean("gpu_failed", v).commit() }

    /** How many voice-dataset sessions have been recorded (used only to show progress). */
    var datasetSessions: Int
        get() = sp.getInt("dataset_sessions", 0)
        set(v) { sp.edit().putInt("dataset_sessions", v).apply() }

    /** Calibrated "Hey Maya" pronunciations (BPE token strings produced by the wake-word model). */
    var phrases: List<String>
        get() = (sp.getString("phrases", "") ?: "").split('\n').filter { it.isNotBlank() }
        set(v) { sp.edit().putString("phrases", v.joinToString("\n")).apply() }

    /** "fast" | "balanced" | "smooth": how answers are cut up and joined for the voice (see tts_style.h). */
    var voiceStyle: String
        get() = sp.getString("voice_style", "balanced") ?: "balanced"
        set(v) { sp.edit().putString("voice_style", v).apply() }

    /** Log-scale map 0..100 -> trigger threshold 0.30 .. 0.02 (host tests: ~0.06-0.10 was the sweet spot). */
    /** 0 = sherpa keyword spotter (default), 1 = trained "Hey Maya" model (Maya-Wake). */
    var wakeEngine: Int
        get() = sp.getInt("wake_engine", 0)
        set(v) { sp.edit().putInt("wake_engine", if (v == 1) 1 else 0).apply() }

    /**
     * Threshold for the trained model, derived from the same slider. The model's own scores are
     * extremely bimodal (clean "Hey Maya" ~0.9999, everything else < 0.01), so this curve is anchored
     * on the value picked during training validation: 55 (the default) = 0.9976, which measured
     * 28/28 recall and 0 false wakes per hour on the held-out data. Sliding right trades margin for
     * sensitivity: 100 = 0.84 (the point where false wakes appeared in validation).
     */
    val wakeNetThreshold: Float
        get() {
            val s = sensitivity / 100.0
            return if (s >= 0.55) {
                (0.9976 - (s - 0.55) / 0.45 * (0.9976 - 0.84)).toFloat()
            } else {
                (0.9976 + (0.55 - s) / 0.55 * (0.9999 - 0.9976)).toFloat()
            }
        }

    /** The threshold the active engine will actually use. */
    val activeThreshold: Float
        get() = if (wakeEngine == 1) wakeNetThreshold else wakeThreshold

    val wakeThreshold: Float
        get() = (0.30 * (0.02 / 0.30).pow(sensitivity / 100.0)).toFloat()
}
