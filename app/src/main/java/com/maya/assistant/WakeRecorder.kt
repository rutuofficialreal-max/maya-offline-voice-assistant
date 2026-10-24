package com.maya.assistant

import android.Manifest
import android.annotation.SuppressLint
import android.content.ContentValues
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlin.math.sqrt

/**
 * Records the "voice dataset" used to train Maya's own wake-word model (openWakeWord-style).
 *
 * One "session" opens the microphone for [SECONDS] and cuts it into single utterances with a plain
 * energy detector, writing one 16 kHz / 16-bit / mono WAV per utterance — the format the trainer
 * expects. Only clips that look like a spoken phrase (300 ms .. 2.5 s) are kept, with 150 ms of
 * silence on each side. Everything stays in the app's private folder until [exportZip] is tapped.
 */
object WakeRecorder {
    const val RATE = 16000
    const val SECONDS = 75          // wall-clock length of one session
    private const val FRAME = RATE / 50                 // 20 ms analysis frame (320 samples)
    private const val PRE_ROLL = 8                      // 160 ms kept before the onset
    private const val START_RUN = 3                     // 60 ms of energy starts a clip
    private const val END_SILENCE = 28                  // 560 ms of silence ends it
    private const val LEAD_IN = 5                       // extra 100 ms in front of the onset
    private const val TAIL = 9                          // extra 180 ms after the end
    private const val MIN_CLIP_MS = 300
    private const val MAX_CLIP_MS = 2500
    private const val ABS_FLOOR = 320.0                 // ~-40 dBFS

    class Session(val dir: File, val clips: Int, val skipped: Int, val peak: Int)

    fun rootDir(ctx: Context): File = File(ctx.filesDir, "wake_training")

    fun sessions(ctx: Context): List<File> =
        rootDir(ctx).listFiles()?.filter { it.isDirectory && it.name.startsWith("session_") }?.sortedBy { it.name }
            ?: emptyList()

    fun clipCount(ctx: Context): Int = sessions(ctx).sumOf { dir -> dir.listFiles { f -> f.name.endsWith(".wav") }?.size ?: 0 }

    fun nextSessionNumber(ctx: Context): Int {
        val used = sessions(ctx).mapNotNull { it.name.removePrefix("session_").toIntOrNull() }
        return (used.maxOrNull() ?: 0) + 1
    }

    /**
     * Records one session. [onClip] is called after each saved utterance (clip index, total so far),
     * [onTick] once a second with the elapsed seconds, so the UI can keep the user informed.
     * Returns null if the microphone could not be opened or permission is missing.
     */
    @SuppressLint("MissingPermission")
    fun recordSession(
        ctx: Context,
        seconds: Int = SECONDS,
        onClip: (index: Int, total: Int) -> Unit,
        onTick: (elapsedSec: Int) -> Unit,
    ): Session? {
        if (ctx.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) return null
        val minBuf = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        if (minBuf <= 0) return null
        val rec = AudioRecord.Builder()
            .setAudioSource(MediaRecorder.AudioSource.VOICE_RECOGNITION)
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(RATE)
                    .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                    .build()
            )
            .setBufferSizeInBytes(maxOf(minBuf, RATE * 2))
            .build()
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            return null
        }

        val dir = File(rootDir(ctx), "session_%02d".format(nextSessionNumber(ctx))).apply { mkdirs() }
        val buf = ShortArray(FRAME)
        val preRoll = ArrayDeque<ShortArray>()
        var current = ArrayList<Short>()
        var voicedRun = 0
        var silentRun = 0
        var inClip = false
        var clips = 0
        var skipped = 0
        var peak = 0
        var lastTick = 0L

        try {
            rec.startRecording()
            val started = System.currentTimeMillis()
            while (true) {
                val elapsed = ((System.currentTimeMillis() - started) / 1000).toInt()
                if (elapsed >= seconds) break
                if (elapsed.toLong() != lastTick) {
                    lastTick = elapsed.toLong()
                    onTick(elapsed)
                }
                val n = rec.read(buf, 0, FRAME)
                if (n <= 0) continue
                if (n < FRAME) continue

                var sum = 0.0
                for (i in 0 until FRAME) {
                    val v = buf[i]
                    if (v > peak) peak = v.toInt()
                    if (-v > peak) peak = -v.toInt()
                    sum += v.toDouble() * v
                }
                val rms = sqrt(sum / FRAME)

                if (!inClip) {
                    preRoll.addLast(buf.copyOf())
                    if (preRoll.size > PRE_ROLL + 1) preRoll.removeFirst()
                    if (rms > ABS_FLOOR) {
                        if (++voicedRun >= START_RUN) {
                            // onset: open a clip with the pre-roll in front
                            inClip = true
                            silentRun = 0
                            current = ArrayList(FRAME * 64)
                            val keep = preRoll.toList().takeLast(PRE_ROLL + LEAD_IN)
                            for (chunk in keep) for (s in chunk) current.add(s)
                            preRoll.clear()
                        }
                    } else {
                        voicedRun = 0
                    }
                } else {
                    for (s in buf) current.add(s)
                    if (rms > ABS_FLOOR) silentRun = 0 else ++silentRun
                    val ms = current.size * 1000 / RATE
                    val done = silentRun >= END_SILENCE || ms >= MAX_CLIP_MS
                    if (done) {
                        // keep a little tail, drop the long trailing silence
                        val keepSamples = current.size - (silentRun - TAIL).coerceAtLeast(0) * FRAME
                        val clip = if (keepSamples in 1..current.size) current.subList(0, keepSamples).toShortArray()
                        else current.toShortArray()
                        val clipMs = clip.size * 1000 / RATE
                        if (clipMs in MIN_CLIP_MS..(MAX_CLIP_MS + 400)) {
                            clips++
                            writeWav(File(dir, "clip_%03d.wav".format(clips)), clip)
                            onClip(clips, clips)
                        } else {
                            skipped++
                        }
                        current = ArrayList()
                        inClip = false
                        voicedRun = 0
                        silentRun = 0
                    }
                }
            }
        } catch (e: Exception) {
            android.util.Log.w("MayaWakeRec", "session interrupted", e)
        } finally {
            try {
                rec.stop()
            } catch (_: IllegalStateException) {
            }
            rec.release()
        }
        // a clip still open when the timer ran out
        if (inClip && current.size * 1000 / RATE >= MIN_CLIP_MS) {
            clips++
            writeWav(File(dir, "clip_%03d.wav".format(clips)), current.toShortArray())
            onClip(clips, clips)
        }
        return Session(dir, clips, skipped, peak)
    }

    // ------------------------------------------------------------------ WAV / ZIP

    /** 16-bit mono PCM WAV, the format the training notebook reads without any conversion. */
    fun writeWav(file: File, pcm: ShortArray) {
        val dataBytes = pcm.size * 2
        FileOutputStream(file).use { out ->
            val header = ByteArray(44)
            fun putStr(off: Int, s: String) { for (i in s.indices) header[off + i] = s[i].code.toByte() }
            fun putInt(off: Int, v: Int) {
                header[off] = (v and 0xFF).toByte()
                header[off + 1] = (v ushr 8 and 0xFF).toByte()
                header[off + 2] = (v ushr 16 and 0xFF).toByte()
                header[off + 3] = (v ushr 24 and 0xFF).toByte()
            }
            fun putShort(off: Int, v: Int) {
                header[off] = (v and 0xFF).toByte()
                header[off + 1] = (v ushr 8 and 0xFF).toByte()
            }
            putStr(0, "RIFF"); putInt(4, 36 + dataBytes); putStr(8, "WAVE")
            putStr(12, "fmt "); putInt(16, 16); putShort(20, 1); putShort(22, 1)
            putInt(24, RATE); putInt(28, RATE * 2); putShort(32, 2); putShort(34, 16)
            putStr(36, "data"); putInt(40, dataBytes)
            out.write(header)
            val bytes = ByteArray(dataBytes)
            for (i in pcm.indices) {
                bytes[i * 2] = (pcm[i].toInt() and 0xFF).toByte()
                bytes[i * 2 + 1] = (pcm[i].toInt() shr 8 and 0xFF).toByte()
            }
            out.write(bytes)
        }
    }

    /**
     * Zips every recorded session into Downloads/Maya (and the app's external folder as a fallback),
     * so it can be uploaded to Kaggle or shared. Returns the readable path, or null on failure.
     */
    fun exportZip(ctx: Context): String? {
        val dirs = sessions(ctx)
        if (dirs.isEmpty()) return null
        val name = "Maya-wake-voice-%s.zip".format(java.text.SimpleDateFormat("yyyyMMdd-HHmm", java.util.Locale.US).format(java.util.Date()))

        fun write(target: OutputStream) {
            ZipOutputStream(BufferedOutputStream(target)).use { zip ->
                zip.putNextEntry(ZipEntry("README.txt"))
                val readme = buildString {
                    append("Maya wake-word voice dataset\n")
                    append("sessions: ${dirs.size}\n")
                    append("clips: ${dirs.sumOf { d -> d.listFiles { f -> f.name.endsWith(".wav") }?.size ?: 0 }}\n")
                    append("format: 16 kHz, 16-bit, mono WAV, one utterance per file\n")
                    append("device: ${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE}\n")
                }
                zip.write(readme.toByteArray())
                zip.closeEntry()
                for (d in dirs) {
                    for (f in d.listFiles { f -> f.name.endsWith(".wav") }?.sortedBy { it.name } ?: emptyList()) {
                        zip.putNextEntry(ZipEntry("${d.name}/${f.name}"))
                        f.inputStream().use { it.copyTo(zip) }
                        zip.closeEntry()
                    }
                }
            }
        }

        var where: String? = null
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                val values = ContentValues().apply {
                    put(MediaStore.Downloads.DISPLAY_NAME, name)
                    put(MediaStore.Downloads.MIME_TYPE, "application/zip")
                    put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/Maya")
                }
                val uri = ctx.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                if (uri != null) {
                    ctx.contentResolver.openOutputStream(uri)?.use { write(it) }
                    where = "Download/Maya/$name"
                }
            }
        } catch (e: Exception) {
            android.util.Log.w("MayaWakeRec", "MediaStore export failed", e)
        }
        try {
            val alt = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, name)
            FileOutputStream(alt).use { write(it) }
            if (where == null) where = alt.absolutePath
        } catch (e: Exception) {
            android.util.Log.w("MayaWakeRec", "app-folder export failed", e)
        }
        return where
    }
}
