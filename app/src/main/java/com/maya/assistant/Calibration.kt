package com.maya.assistant

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import kotlin.math.abs

/** Records short clips for wake-word calibration (16 kHz mono, same audio source as the engine). */
object Calibration {
    const val SAMPLE_RATE = 16000

    /** Blocks for [seconds]. Returns null if the microphone could not be opened or delivered too little. */
    @SuppressLint("MissingPermission")
    fun record(seconds: Float): ShortArray? {
        val total = (SAMPLE_RATE * seconds).toInt()
        val minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        if (minBuf <= 0) return null
        val rec = AudioRecord.Builder()
            .setAudioSource(MediaRecorder.AudioSource.VOICE_RECOGNITION)
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                    .build()
            )
            .setBufferSizeInBytes(maxOf(minBuf, SAMPLE_RATE * 2))
            .build()
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            return null
        }
        val out = ShortArray(total)
        var n = 0
        try {
            rec.startRecording()
            while (n < total) {
                val r = rec.read(out, n, total - n)
                if (r <= 0) break
                n += r
            }
        } finally {
            try { rec.stop() } catch (_: IllegalStateException) { }
            rec.release()
        }
        return if (n >= total / 2) out.copyOf(n) else null
    }

    fun peak(pcm: ShortArray): Int {
        var p = 0
        for (s in pcm) p = maxOf(p, abs(s.toInt()))
        return p
    }
}
