package com.maya.assistant

/** Pure helpers for calibrated wake phrases: BPE token strings such as "▁HE Y ▁MY A". */
object PhraseBook {
    const val MAX_PHRASES = 4
    private const val MARKER = "\u2581"

    private fun clean(p: String): String? {
        // Phrases are stored and sent to native code one per line: an embedded line break means corruption.
        if ('\n' in p || '\r' in p) return null
        val t = p.trim().replace(Regex("\\s+"), " ")
        return if (t.isEmpty() || t.length > 160) null else t
    }

    /**
     * Newest first, de-duplicated, capped at [max]. Re-calibrating therefore replaces the oldest phrases
     * (the old code kept the oldest four, so after four phrases a new calibration changed nothing).
     * [found] is in capture order, so its last element is the newest.
     */
    fun merge(existing: List<String>, found: List<String>, max: Int = MAX_PHRASES): List<String> {
        val out = LinkedHashSet<String>()
        for (p in found.asReversed()) clean(p)?.let { out.add(it) }
        for (p in existing) clean(p)?.let { out.add(it) }
        return out.take(max)
    }

    /** "▁HE Y ▁MY A" -> "HEY MYA" */
    fun display(tokens: String): String = tokens.replace(" ", "").replace(MARKER, " ").trim()
}
