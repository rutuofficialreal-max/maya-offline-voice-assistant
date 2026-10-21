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

    /** Calibrated "Hey Maya" pronunciations (BPE token strings produced by the wake-word model). */
    var phrases: List<String>
        get() = (sp.getString("phrases", "") ?: "").split('\n').filter { it.isNotBlank() }
        set(v) { sp.edit().putString("phrases", v.joinToString("\n")).apply() }

    /** Log-scale map 0..100 -> trigger threshold 0.30 .. 0.02 (host tests: ~0.06-0.10 was the sweet spot). */
    val wakeThreshold: Float
        get() = (0.30 * (0.02 / 0.30).pow(sensitivity / 100.0)).toFloat()
}
