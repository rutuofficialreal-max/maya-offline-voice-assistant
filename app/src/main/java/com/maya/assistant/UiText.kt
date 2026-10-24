package com.maya.assistant

import java.util.Locale

/**
 * Every user-facing string that is built with format() or from several parts lives here, as pure
 * functions, so a plain JVM test can exercise each of them. (A '%' inside a format template crashed the
 * app at launch in v1.0.0; a template with an unescaped '%' now fails the CI check and these tests.)
 */
object UiText {
    fun sensitivityLabel(percent: Int, threshold: Float, live: Boolean): String =
        String.format(
            Locale.US, "Sensitivity %d%%  (trigger threshold %.3f, %s)", percent, threshold,
            if (live) "applies immediately" else "applies when Maya starts"
        )

    fun calibrationSummary(phrases: List<String>, status: String, calibrating: Boolean): String = when {
        calibrating -> status
        phrases.isEmpty() && status.isNotEmpty() -> status
        phrases.isEmpty() ->
            "Not calibrated yet. Recommended: say \u201CHey Maya\u201D three times so Maya learns how you pronounce it (about 20 seconds)."
        else -> "Calibrated: " + phrases.joinToString("  \u00B7  ") { PhraseBook.display(it) } +
            (if (status.isNotEmpty()) "\n$status" else "")
    }

    fun calibrationPrompt(index: Int, total: Int): String = "Say \u201CHey Maya\u201D now  ($index of $total)"

    fun calibrationHeard(heard: String, index: Int, total: Int): String = "Heard \u201C$heard\u201D \u2713  ($index of $total)"

    fun voiceStyleHint(style: String, running: Boolean): String {
        val base = when (style) {
            "fast" -> "Starts speaking after the first clause. Quickest, but the voice restarts at each clause."
            "smooth" -> "Waits for the full answer, then speaks it in one go. Slowest start, no gaps even on a slow CPU."
            else -> "Speaks whole sentences with natural pauses. Starts after the first sentence."
        }
        return if (running) "$base  (applies next start)" else base
    }
}
