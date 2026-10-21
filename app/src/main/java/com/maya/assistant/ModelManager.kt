package com.maya.assistant

import android.content.Context
import java.io.File

/** What gets downloaded after install, where it lives, and the native engine configuration. */
object ModelManager {
    enum class Kind { FILE, TAR_BZ2 }

    data class Item(
        val id: String,
        val title: String,
        val url: String,
        val kind: Kind,
        val dir: String,        // sub-directory of filesDir/models
        val check: String,      // file (relative to dir) that must exist when the item is installed
        val bytes: Long,        // approximate download size, used for progress
        val fileName: String = "",   // Kind.FILE: final file name inside dir
        val sha256: String? = null,  // verified after download when known
    )

    private const val SHERPA = "https://github.com/k2-fsa/sherpa-onnx/releases/download"
    private const val HF = "https://huggingface.co"

    const val KWS_ENCODER = "encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
    const val KWS_DECODER = "decoder-epoch-12-avg-2-chunk-16-left-64.onnx"
    const val KWS_JOINER = "joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
    const val TTS_MODEL = "en_GB-jenny_dioco-medium.onnx"
    const val STT_FILE = "ggml-base.en-q5_1.bin"
    const val LLM_FILE = "Llama-3.2-1B-Instruct-Q4_K_M.gguf"

    val ITEMS = listOf(
        Item("kws", "Wake word model", "$SHERPA/kws-models/sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2",
            Kind.TAR_BZ2, "kws", "tokens.txt", 17_626_723L),
        Item("vad", "Voice activity detector", "$SHERPA/asr-models/silero_vad.onnx",
            Kind.FILE, "vad", "silero_vad.onnx", 643_854L, fileName = "silero_vad.onnx"),
        Item("tts", "Maya's voice (Jenny, VITS)", "$SHERPA/tts-models/vits-piper-en_GB-jenny_dioco-medium.tar.bz2",
            Kind.TAR_BZ2, "tts", TTS_MODEL, 67_225_842L),
        Item("stt", "Speech recognition (Whisper base.en, 5-bit)", "$HF/ggerganov/whisper.cpp/resolve/main/$STT_FILE",
            Kind.FILE, "stt", STT_FILE, 59_721_011L, fileName = STT_FILE,
            sha256 = "4baf70dd0d7c4247ba2b81fafd9c01005ac77c2f9ef064e00dcf195d0e2fdd2f"),
        Item("llm", "Language model (Llama 3.2 1B, 4-bit)", "$HF/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/$LLM_FILE",
            Kind.FILE, "llm", LLM_FILE, 808_000_000L, fileName = LLM_FILE,
            sha256 = "6f85a640a97cf2bf5b8e764087b1e83da0fdb51d7c9fab7d0fece9385611df83"),
    )

    fun rootDir(ctx: Context): File = File(ctx.filesDir, "models")
    fun itemDir(ctx: Context, item: Item): File = File(rootDir(ctx), item.dir)

    fun isReady(ctx: Context, item: Item): Boolean {
        val f = File(itemDir(ctx, item), item.check)
        if (!f.isFile || f.length() <= 0) return false
        return item.kind == Kind.FILE || File(itemDir(ctx, item), ".complete").exists()
    }

    fun missing(ctx: Context): List<Item> = ITEMS.filter { !isReady(ctx, it) }
    fun allReady(ctx: Context): Boolean = missing(ctx).isEmpty()
    fun kwsReady(ctx: Context): Boolean = isReady(ctx, ITEMS.first { it.id == "kws" })

    fun deleteAll(ctx: Context) {
        rootDir(ctx).deleteRecursively()
    }

    private fun path(ctx: Context, vararg parts: String): String = File(rootDir(ctx), parts.joinToString("/")).absolutePath

    /** Newline separated key=value pairs, parsed by EngineConfig::parse in the native core. */
    fun buildConfig(ctx: Context, prefs: Prefs, useGpu: Boolean): String = buildString {
        fun kv(k: String, v: Any) { append(k).append('=').append(v).append('\n') }
        kv("kws.encoder", path(ctx, "kws", KWS_ENCODER))
        kv("kws.decoder", path(ctx, "kws", KWS_DECODER))
        kv("kws.joiner", path(ctx, "kws", KWS_JOINER))
        kv("kws.tokens", path(ctx, "kws", "tokens.txt"))
        kv("kws.threshold", "%.4f".format(java.util.Locale.US, prefs.wakeThreshold))
        kv("kws.boost", "1.5")
        for (p in prefs.phrases) kv("kws.phrase", p)
        kv("vad.model", path(ctx, "vad", "silero_vad.onnx"))
        kv("vad.end_silence_ms", 800)
        kv("whisper.model", path(ctx, "stt", STT_FILE))
        kv("whisper.adaptive_ctx", 1)  // set to 0 if transcripts look poor (slower, uses the full 30 s window)
        kv("llm.model", path(ctx, "llm", LLM_FILE))
        kv("llm.gpu_layers", if (useGpu) 99 else 0)
        kv("tts.model", path(ctx, "tts", TTS_MODEL))
        kv("tts.tokens", path(ctx, "tts", "tokens.txt"))
        kv("tts.data_dir", path(ctx, "tts", "espeak-ng-data"))
        kv("pin_big_cores", 1)
    }
}
