package com.maya.assistant

import kotlin.math.PI
import kotlin.math.sin
import kotlin.random.Random

private var failures = 0
private fun check(name: String, ok: Boolean, detail: String = "") {
    if (!ok) { failures++; println("FAIL: $name $detail") }
}

private fun tone(ms: Int, amp: Int, hz: Double = 220.0): ShortArray =
    ShortArray(SampleTrimmer.RATE * ms / 1000) { i -> (amp * sin(2 * PI * hz * i / SampleTrimmer.RATE)).toInt().toShort() }

private fun noise(ms: Int, amp: Int, rnd: Random): ShortArray =
    ShortArray(SampleTrimmer.RATE * ms / 1000) { (rnd.nextInt(-amp, amp + 1)).toShort() }

fun main() {
    // ---------------- UiText: every formatted string, with awkward values
    for (pct in listOf(0, 1, 55, 100)) for (thr in listOf(0.0f, 0.02f, 0.072f, 0.3f)) for (live in listOf(true, false)) {
        val s = try { UiText.sensitivityLabel(pct, thr, live) } catch (e: Exception) { "EXC ${e.javaClass.simpleName}"; null }
        check("sensitivityLabel($pct,$thr,$live) must not throw", s != null)
        if (s != null) check("sensitivity text", s.startsWith("Sensitivity $pct%") && s.contains("threshold") && s.contains(if (live) "immediately" else "when Maya starts"), s)
    }
    check("label exact", UiText.sensitivityLabel(55, 0.0724f, true) == "Sensitivity 55%  (trigger threshold 0.072, applies immediately)")
    check("calibration summary empty", UiText.calibrationSummary(emptyList(), "", false).startsWith("Not calibrated yet"))
    check("calibration summary status only", UiText.calibrationSummary(emptyList(), "Get ready", false) == "Get ready")
    check("calibration summary with phrases", UiText.calibrationSummary(listOf("\u2581HE Y \u2581MY A", "\u2581HE Y \u2581MA Y A"), "Saved.", false) == "Calibrated: HEY MYA  \u00B7  HEY MAYA\nSaved.")
    check("calibration while calibrating", UiText.calibrationSummary(listOf("x"), "Listening", true) == "Listening")
    check("prompt/heard", UiText.calibrationPrompt(2, 3).contains("(2 of 3)") && UiText.calibrationHeard("HEY MYA", 1, 3).contains("HEY MYA"))
    for (st in listOf("fast", "balanced", "smooth", "unknown")) for (r in listOf(true, false)) check("voiceStyleHint($st,$r)", UiText.voiceStyleHint(st, r).isNotEmpty())

    // ---------------- PhraseBook
    val a = "\u2581HE Y \u2581MY A"; val b = "\u2581HE Y \u2581MA Y A"; val c = "\u2581HE Y \u2581MY R"; val d = "\u2581HE Y \u2581MY ER"; val e = "\u2581PAY \u2581MY ER"
    check("display", PhraseBook.display(a) == "HEY MYA")
    check("merge keeps NEWEST (old code kept the oldest four)", PhraseBook.merge(listOf(a, b, c, d), listOf(e)) == listOf(e, a, b, c))
    check("merge dedupes", PhraseBook.merge(listOf(a, b), listOf(a, a)) == listOf(a, b))
    check("merge: last captured is newest", PhraseBook.merge(emptyList(), listOf(a, b, c)) == listOf(c, b, a))
    check("merge cap", PhraseBook.merge(listOf(a, b, c, d), listOf(e, "x y", "z w")).size == PhraseBook.MAX_PHRASES)
    check("merge drops blank and newline entries", PhraseBook.merge(listOf("", "  ", "a\nb"), listOf(a)) == listOf(a))
    check("merge whitespace normalised", PhraseBook.merge(emptyList(), listOf("  \u2581HE   Y  ")) == listOf("\u2581HE Y"))

    // ---------------- SampleTrimmer
    val rnd = Random(3)
    // typical clip: 0.5 s room noise, 0.8 s "speech", 1.1 s noise
    val clip = noise(500, 60, rnd) + tone(800, 6000) + noise(1100, 60, rnd)
    val r = SampleTrimmer.trim(clip)
    check("trim finds speech", r != null)
    if (r != null) {
        val ms = r.pcm.size * 1000 / SampleTrimmer.RATE
        check("trimmed length ~ 0.8 s + margins (was ${clip.size * 1000 / SampleTrimmer.RATE} ms)", ms in 900..1250, "got $ms ms")
        check("voiced duration", r.voicedMs in 700..900, "got ${r.voicedMs}")
    }
    // beep bleed at the start (loud 120 ms tone) followed by silence then speech: trimmer keeps both (energy only), but pure silence is rejected
    check("pure noise rejected", SampleTrimmer.trim(noise(2400, 80, rnd)) == null)
    check("digital silence rejected", SampleTrimmer.trim(ShortArray(38400)) == null)
    check("too short rejected", SampleTrimmer.trim(noise(500, 50, rnd) + tone(120, 6000) + noise(900, 50, rnd)) == null)
    check("tiny clip rejected", SampleTrimmer.trim(ShortArray(100)) == null)
    // quiet speaker in a noisy room (noise floor 800): speech at 5x that still found
    check("noisy room", SampleTrimmer.trim(noise(500, 800, rnd) + tone(700, 9000) + noise(1000, 800, rnd)) != null)
    // loud background throughout: threshold adapts, constant tone-like noise is not 'speech' above 3.5x its own floor
    check("loud constant noise rejected", SampleTrimmer.trim(noise(2400, 6000, rnd)) == null)

    println(if (failures == 0) "all JVM pure-logic tests passed" else "$failures JVM TEST(S) FAILED")
    if (failures != 0) System.exit(1)
}
