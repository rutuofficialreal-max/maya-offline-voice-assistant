package com.maya.assistant

import kotlin.math.sqrt

/**
 * Trims a calibration recording to the spoken part with a plain energy detector, so leading/trailing
 * silence (and the tail of any cue sound) never reaches the recogniser. Pure Kotlin, no Android.
 */
object SampleTrimmer {
    const val RATE = 16000
    private const val FRAME = RATE / 50          // 20 ms
    private const val MARGIN_BEFORE = 8          // 160 ms kept before the first voiced frame
    private const val MARGIN_AFTER = 9           // 180 ms kept after the last voiced frame
    private const val MIN_RUN = 3                // 60 ms above the threshold counts as speech
    private const val MIN_VOICED_MS = 250
    private const val ABS_FLOOR = 400.0          // about -38 dBFS: ignore anything quieter

    class Result(val pcm: ShortArray, val voicedMs: Int)

    /** Returns null when there is no speech-like energy (or it is shorter than 250 ms). */
    fun trim(pcm: ShortArray): Result? {
        val frames = pcm.size / FRAME
        if (frames < 10) return null
        val rms = DoubleArray(frames) { i ->
            var s = 0.0
            val base = i * FRAME
            for (k in 0 until FRAME) {
                val v = pcm[base + k].toDouble()
                s += v * v
            }
            sqrt(s / FRAME)
        }
        // The 25th percentile is a noise-floor estimate that the voiced frames cannot pull up.
        val noise = rms.sortedArray()[(frames * 0.25).toInt()]
        val threshold = maxOf(noise * 3.5, ABS_FLOOR)

        var first = -1
        var last = -1
        var run = 0
        for (i in 0 until frames) {
            if (rms[i] > threshold) {
                run++
                if (run >= MIN_RUN) {
                    if (first < 0) first = i - MIN_RUN + 1
                    last = i
                }
            } else {
                run = 0
            }
        }
        if (first < 0) return null
        val voicedMs = (last - first + 1) * 20
        if (voicedMs < MIN_VOICED_MS) return null
        val from = maxOf(0, first - MARGIN_BEFORE) * FRAME
        val to = minOf(frames, last + 1 + MARGIN_AFTER) * FRAME
        return Result(pcm.copyOfRange(from, to), voicedMs)
    }
}
