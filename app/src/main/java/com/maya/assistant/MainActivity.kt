package com.maya.assistant

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.util.concurrent.Executors

/** One screen: model download, wake-word calibration, start/stop, settings and a live log. */
class MainActivity : Activity() {
    private lateinit var prefs: Prefs
    private val worker = Executors.newSingleThreadExecutor()
    private var afterPermissions: (() -> Unit)? = null
    @Volatile private var calibrating = false

    // palette
    private val cBg = Color.parseColor("#0E1116")
    private val cSurface = Color.parseColor("#171B22")
    private val cButton = Color.parseColor("#252B36")
    private val cText = Color.parseColor("#E6E9EE")
    private val cMuted = Color.parseColor("#8B95A5")
    private val cAccent = Color.parseColor("#7C9CFF")
    private val cListen = Color.parseColor("#FFC857")
    private val cThink = Color.parseColor("#B58CFF")
    private val cSpeak = Color.parseColor("#4FD1C5")
    private val cError = Color.parseColor("#FF6B6B")

    private lateinit var orb: OrbView
    private lateinit var titleView: TextView
    private lateinit var subView: TextView
    private lateinit var heardView: TextView
    private lateinit var replyView: TextView
    private lateinit var modelsInfo: TextView
    private lateinit var modelsBar: ProgressBar
    private lateinit var modelsBtn: Button
    private lateinit var pauseBtn: Button
    private lateinit var calInfo: TextView
    private lateinit var calBtn: Button
    private lateinit var calReset: Button
    private lateinit var startBtn: Button
    private lateinit var silenceBtn: Button
    private lateinit var gpuSwitch: Switch
    private lateinit var sensLabel: TextView
    private lateinit var datasetInfo: TextView
    private lateinit var datasetBtn: Button
    private lateinit var datasetExport: Button
    private lateinit var styleButtons: List<Button>
    private lateinit var styleHint: TextView
    private lateinit var logView: TextView

    private var reportShown = false
    private var healthyMarked = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        MayaApp.stage(this, "activity:onCreate")
        prefs = Prefs(this)

        // If the previous run died with an exception, show it instead of crashing again.
        val previous = MayaApp.takeCrash(this)
        if (previous != null) {
            showReport(previous)
            return
        }
        try {
            Notifications.ensureChannels(this)
            window.setBackgroundDrawable(ColorDrawable(cBg))
            MayaApp.stage(this, "activity:buildUi")
            setContentView(buildUi())
            MayaApp.stage(this, "activity:ui built")
            MayaApp.previousStartWasUnclean(this)?.let { MayaBus.log("Previous start ended early at: " + it) }
        } catch (t: Throwable) {
            report(t)
        }
    }

    override fun onStart() {
        super.onStart()
        if (reportShown) return
        try {
            MayaApp.stage(this, "activity:onStart")
            MayaBus.listener = { safeRender() }
            render()
            if (!healthyMarked) {
                healthyMarked = true
                MayaApp.markHealthy(this)  // the UI is up: a later crash is not a start-up crash
            }
        } catch (t: Throwable) {
            MayaBus.listener = null
            report(t)
        }
    }

    override fun onStop() {
        MayaBus.listener = null
        super.onStop()
    }

    // ------------------------------------------------------------------------------ crash reporting

    private fun safeRender() {
        if (reportShown) return
        try {
            render()
        } catch (t: Throwable) {
            MayaBus.listener = null
            report(t)
        }
    }

    /** Records [t] and replaces the screen with the report, so the reason is visible on the phone. */
    private fun report(t: Throwable) {
        MayaApp.record(this, Thread.currentThread(), t)
        val text = MayaApp.takeCrash(this) ?: (t.toString() + "\n" + android.util.Log.getStackTraceString(t))
        showReport(text)
    }

    /**
     * Minimal, dependency-free screen: framework views only, no prefs, no bus, no models.
     * The text can be selected, copied and shared, which is how the trace gets back to me.
     */
    private fun showReport(text: String) {
        reportShown = true
        val full = if (text.contains("stack trace")) text else text + "\n\n--- start-up breadcrumbs ---\n" + MayaApp.startupLog(this)

        val body = TextView(this).apply {
            this.text = full
            textSize = 11f
            setTextColor(Color.parseColor("#E6E9EE"))
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
            setPadding(dp(16), dp(12), dp(16), dp(12))
        }
        val share = button("Share this report", primary = true) { shareReport(full) }
        val copy = button("Copy", primary = false) { copyReport(full) }
        val close = button("Close Maya", primary = false) { finish() }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.parseColor("#0E1116"))
            setPadding(0, dp(24), 0, dp(16))
            addView(text("Maya hit an error", 19f, Color.parseColor("#FF6B6B"), true).apply {
                setPadding(dp(16), 0, dp(16), dp(4))
            })
            addView(text(
                "Please send this to whoever builds Maya. It contains no audio and no personal data.",
                12f, Color.parseColor("#8B95A5")
            ).apply { setPadding(dp(16), 0, dp(16), dp(10)) })
            addView(ScrollView(this@MainActivity).apply {
                addView(body)
                layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
            })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(16), dp(10), dp(16), 0)
                addView(share, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
                addView(copy, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 8))
                addView(close, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 8))
            })
        }
        try {
            setContentView(root)
        } catch (_: Throwable) {
        }
    }

    private fun shareReport(text: String) {
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, "Maya crash report")
            putExtra(Intent.EXTRA_TEXT, text)
        }
        try {
            startActivity(Intent.createChooser(send, "Send the Maya report"))
        } catch (_: Throwable) {
            copyReport(text)
        }
    }

    private fun copyReport(text: String) {
        try {
            val cm = getSystemService(android.content.ClipboardManager::class.java)
            cm.setPrimaryClip(android.content.ClipData.newPlainText("Maya report", text))
            Toast.makeText(this, "Copied. Paste it into a message to the Maya developer.", Toast.LENGTH_LONG).show()
        } catch (_: Throwable) {
        }
    }

    // ------------------------------------------------------------------------------------------ UI

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density + 0.5f).toInt()

    private fun rounded(color: Int, radius: Int) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(radius).toFloat()
    }

    private fun text(s: String, sp: Float, color: Int, medium: Boolean = false) = TextView(this).apply {
        text = s
        textSize = sp
        setTextColor(color)
        if (medium) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }

    private fun lp(w: Int, h: Int, top: Int = 0) = LinearLayout.LayoutParams(w, h).apply { topMargin = dp(top) }

    private fun button(label: String, primary: Boolean, onClick: () -> Unit) = Button(this).apply {
        text = label
        isAllCaps = false
        textSize = 15f
        setTextColor(if (primary) Color.parseColor("#0B1020") else cText)
        background = rounded(if (primary) cAccent else cButton, 12)
        stateListAnimator = null
        minHeight = dp(46)
        minimumHeight = dp(46)
        setOnClickListener { onClick() }
    }

    private fun card(title: String, vararg children: View): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        background = rounded(cSurface, 16)
        setPadding(dp(16), dp(14), dp(16), dp(16))
        layoutParams = lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 12)
        addView(text(title.uppercase(), 11f, cMuted, true).apply { letterSpacing = 0.12f })
        for (c in children) addView(c)
    }

    private fun buildUi(): View {
        val match = ViewGroup.LayoutParams.MATCH_PARENT
        val wrap = ViewGroup.LayoutParams.WRAP_CONTENT

        orb = OrbView(this)
        titleView = text("", 22f, cText, true)
        subView = text("", 13f, cMuted)
        val head = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(orb, LinearLayout.LayoutParams(dp(104), dp(104)))
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(12), 0, 0, 0)
                addView(titleView)
                addView(subView, lp(match, wrap, 4))
            }, LinearLayout.LayoutParams(0, wrap, 1f))
        }

        heardView = text("", 14f, cMuted).apply { typeface = Typeface.create(Typeface.SERIF, Typeface.ITALIC) }
        replyView = text("", 17f, cText)
        val convo = card("Conversation", heardView.apply { layoutParams = lp(match, wrap, 8) }, replyView.apply { layoutParams = lp(match, wrap, 6) })

        modelsInfo = text("", 14f, cText)
        modelsBar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000
            progressTintList = ColorStateList.valueOf(cAccent)
        }
        modelsBtn = button("Download models", true) { onDownload() }
        pauseBtn = button("Pause", false) { ModelDownloadService.pause(this) }
        val models = card(
            "Models",
            modelsInfo.apply { layoutParams = lp(match, wrap, 8) },
            modelsBar.apply { layoutParams = lp(match, dp(8), 10) },
            LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(modelsBtn, LinearLayout.LayoutParams(0, wrap, 1f))
                addView(pauseBtn, LinearLayout.LayoutParams(0, wrap, 1f).apply { leftMargin = dp(8) })
                layoutParams = lp(match, wrap, 12)
            }
        )

        calInfo = text("", 14f, cText)
        calBtn = button("Calibrate \u201CHey Maya\u201D", true) { onCalibrate() }
        calReset = button("Reset", false) { prefs.phrases = emptyList(); render() }
        val wake = card(
            "Wake word",
            calInfo.apply { layoutParams = lp(match, wrap, 8) },
            LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(calBtn, LinearLayout.LayoutParams(0, wrap, 2f))
                addView(calReset, LinearLayout.LayoutParams(0, wrap, 1f).apply { leftMargin = dp(8) })
                layoutParams = lp(match, wrap, 12)
            },
            text(UiText.sensitivityLabel(prefs.sensitivity, prefs.wakeThreshold, MayaBus.running), 12f, cMuted).apply { layoutParams = lp(match, wrap, 14) }.also { sensLabel = it },
            SeekBar(this).apply {
                max = 100
                progress = prefs.sensitivity
                progressTintList = ColorStateList.valueOf(cAccent)
                thumbTintList = ColorStateList.valueOf(cAccent)
                setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                    override fun onProgressChanged(s: SeekBar?, p: Int, fromUser: Boolean) {
                        if (fromUser) prefs.sensitivity = p
                        sensLabel.text = UiText.sensitivityLabel(p, prefs.wakeThreshold, MayaBus.running)
                    }
                    override fun onStartTrackingTouch(s: SeekBar?) {}
                    override fun onStopTrackingTouch(s: SeekBar?) {
                        if (MayaBus.running) MayaService.reloadWake(this@MainActivity)  // applied live, no restart
                    }
                })
            }
        )

        datasetInfo = text("", 13f, cMuted)
        datasetBtn = button("Record a 75 s session", true) { onRecordSession() }
        datasetExport = button("Export ZIP", false) { onExportDataset() }
        val dataset = card(
            "Voice dataset (your own wake word)",
            datasetInfo.apply { layoutParams = lp(match, wrap, 8) },
            LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(datasetBtn, LinearLayout.LayoutParams(0, wrap, 2f))
                addView(datasetExport, LinearLayout.LayoutParams(0, wrap, 1f).apply { leftMargin = dp(8) })
                layoutParams = lp(match, wrap, 12)
            }
        )

        startBtn = button("Start Maya", true) { onStartStop() }
        silenceBtn = button("Silence", false) { MayaService.interrupt(this) }
        val assistant = card(
            "Assistant",
            LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(startBtn, LinearLayout.LayoutParams(0, wrap, 2f))
                addView(silenceBtn, LinearLayout.LayoutParams(0, wrap, 1f).apply { leftMargin = dp(8) })
                layoutParams = lp(match, wrap, 10)
            }
        )

        gpuSwitch = Switch(this).apply {
            setTextColor(cText)
            isChecked = prefs.useGpu && !prefs.gpuFailed
            setOnCheckedChangeListener { _, checked ->
                prefs.useGpu = checked
                if (checked) prefs.gpuFailed = false
                render()
            }
        }
        val styleKeys = listOf("fast", "balanced", "smooth")
        styleButtons = listOf("Fast", "Balanced", "Smooth").mapIndexed { i, label ->
            button(label, false) { prefs.voiceStyle = styleKeys[i]; render() }
        }
        styleHint = text("", 12f, cMuted)
        val styleRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            styleButtons.forEachIndexed { i, b ->
                addView(b, LinearLayout.LayoutParams(0, wrap, 1f).apply { if (i > 0) leftMargin = dp(8) })
            }
            layoutParams = lp(match, wrap, 6)
        }
        val settings = card(
            "Settings",
            Switch(this).apply {
                text = "Download on Wi-Fi only"
                setTextColor(cText)
                isChecked = prefs.wifiOnly
                setOnCheckedChangeListener { _, c -> prefs.wifiOnly = c }
                layoutParams = lp(match, wrap, 8)
            },
            gpuSwitch.apply { layoutParams = lp(match, wrap, 10) },
            text("Voice style", 13f, cText, true).apply { layoutParams = lp(match, wrap, 16) },
            styleRow,
            styleHint.apply { layoutParams = lp(match, wrap, 6) },
            button("Allow background running", false) { onBattery() }.apply { layoutParams = lp(match, wrap, 12) },
            button("Delete downloaded models", false) { confirmDelete() }.apply { layoutParams = lp(match, wrap, 8) }
        )

        logView = text("", 11f, cMuted).apply {
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
            layoutParams = lp(match, wrap, 8)
        }
        val log = card("Log", logView)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), dp(24))
            addView(head)
            addView(convo)
            addView(models)
            addView(wake)
            addView(dataset)
            addView(assistant)
            addView(settings)
            addView(log)
        }
        return ScrollView(this).apply {
            isFillViewport = true
            addView(root)
            // targetSdk 35 draws edge to edge: keep the content clear of the system bars
            setOnApplyWindowInsetsListener { v, insets ->
                val b = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                v.setPadding(b.left, b.top, b.right, b.bottom)
                WindowInsets.CONSUMED
            }
        }
    }

    // --------------------------------------------------------------------------------------- render

    private fun render() {
        val st = MayaBus.state
        val running = MayaBus.running
        val ready = MayaBus.ready
        val allReady = ModelManager.allReady(this)

        val (color, pulse) = when {
            !running -> cMuted to false
            !ready -> cAccent to true
            st == NativeBridge.STATE_LISTENING -> cListen to true
            st == NativeBridge.STATE_TRANSCRIBING || st == NativeBridge.STATE_THINKING -> cThink to true
            st == NativeBridge.STATE_SPEAKING -> cSpeak to true
            st == NativeBridge.STATE_ERROR -> cError to false
            else -> cAccent to false
        }
        orb.setLook(color, pulse)
        titleView.text = when {
            !running -> "Maya is off"
            !ready -> "Starting\u2026"
            st == NativeBridge.STATE_LISTENING -> "Listening\u2026"
            st == NativeBridge.STATE_TRANSCRIBING -> "Transcribing\u2026"
            st == NativeBridge.STATE_THINKING -> "Thinking\u2026"
            st == NativeBridge.STATE_SPEAKING -> "Speaking\u2026"
            st == NativeBridge.STATE_ERROR -> "Error"
            else -> "Say \u201CHey Maya\u201D"
        }
        subView.text = when {
            !running && allReady -> "Tap Start Maya to begin"
            !running -> "Download the models first"
            !ready -> MayaBus.statusLine
            else -> MayaBus.backendLine
        }
        heardView.text = if (MayaBus.lastHeard.isEmpty()) "Nothing yet. Say \u201CHey Maya\u201D, then your question." else "\u201C${MayaBus.lastHeard}\u201D"
        replyView.text = MayaBus.lastReply

        // models
        val missing = ModelManager.missing(this)
        val dl = MayaBus.download
        val partial = File(ModelManager.rootDir(this), ".downloads").listFiles()?.isNotEmpty() == true
        modelsInfo.text = when {
            dl.active -> "${dl.title}\n${dl.detail}"
            missing.isEmpty() -> "All models installed. Maya now works fully offline."
            dl.error != null -> dl.error
            dl.paused -> dl.detail
            else -> "Not downloaded yet: about ${missing.sumOf { it.bytes } / 1_048_576} MB. They are stored on this phone only; the app itself stays small."
        }
        modelsBar.visibility = if (dl.active) View.VISIBLE else View.GONE
        modelsBar.progress = (dl.fraction * 1000).toInt()
        modelsBtn.visibility = if (dl.active || missing.isEmpty()) View.GONE else View.VISIBLE
        modelsBtn.text = if (partial || dl.paused || dl.error != null) "Resume download" else "Download models"
        pauseBtn.visibility = if (dl.active) View.VISIBLE else View.GONE

        // wake word
        val phrases = prefs.phrases
        calInfo.text = UiText.calibrationSummary(phrases, MayaBus.calibration, calibrating)
        calBtn.isEnabled = !calibrating && ModelManager.kwsReady(this)  // a running Maya is paused and restarted automatically
        calBtn.alpha = if (calBtn.isEnabled) 1f else 0.45f
        calReset.visibility = if (phrases.isEmpty()) View.GONE else View.VISIBLE
        sensLabel.text = UiText.sensitivityLabel(prefs.sensitivity, prefs.wakeThreshold, running)

        // assistant
        startBtn.text = if (running) "Stop Maya" else "Start Maya"
        startBtn.isEnabled = (running || allReady) && !calibrating
        startBtn.alpha = if (startBtn.isEnabled) 1f else 0.45f
        silenceBtn.visibility = if (st == NativeBridge.STATE_THINKING || st == NativeBridge.STATE_SPEAKING) View.VISIBLE else View.GONE

        // voice dataset
        val sessions = WakeRecorder.sessions(this)
        val clips = WakeRecorder.clipCount(this)
        datasetInfo.text = when {
            recordingDataset -> MayaBus.dataset
            clips == 0 -> "For a wake word of your own: record 3 sessions (about 4 minutes in total) and export the ZIP. " +
                "Each session just asks you to repeat \u201CHey Maya\u201D with a short pause between repeats."
            sessions.size >= 3 -> "$clips clips in ${sessions.size} sessions. Tap Export ZIP and send the file on."
            else -> "$clips clips in ${sessions.size} session(s) \u00B7 ${3 - sessions.size} more recommended. " +
                MayaBus.dataset
        }
        datasetBtn.isEnabled = !recordingDataset && !calibrating
        datasetBtn.alpha = if (datasetBtn.isEnabled) 1f else 0.45f
        datasetBtn.text = if (recordingDataset) "Recording\u2026" else "Record a ${WakeRecorder.SECONDS} s session"
        datasetExport.visibility = if (clips > 0) View.VISIBLE else View.GONE

        // settings
        listOf("fast", "balanced", "smooth").forEachIndexed { i, key ->
            val selected = prefs.voiceStyle == key
            styleButtons[i].background = rounded(if (selected) cAccent else cButton, 12)
            styleButtons[i].setTextColor(if (selected) Color.parseColor("#0B1020") else cText)
        }
        styleHint.text = UiText.voiceStyleHint(prefs.voiceStyle, running)
        gpuSwitch.text = if (prefs.gpuFailed) "GPU (Vulkan): turned off after a crash. Switch to retry" else "Use the GPU (Vulkan) for the language model"
        if (gpuSwitch.isChecked != (prefs.useGpu && !prefs.gpuFailed)) gpuSwitch.isChecked = prefs.useGpu && !prefs.gpuFailed
        logView.text = MayaBus.logText().ifEmpty { "Events appear here." }
    }

    // --------------------------------------------------------------------------------------- actions

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_LONG).show()

    private fun hasMic() = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    /** Asks for the microphone (and, on Android 13+, notification) permission, then runs [then] whatever the answer. */
    private fun withPermissions(then: () -> Unit) {
        val need = ArrayList<String>()
        if (!hasMic()) need += Manifest.permission.RECORD_AUDIO
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            need += Manifest.permission.POST_NOTIFICATIONS
        }
        if (need.isEmpty()) {
            then()
        } else {
            afterPermissions = then
            requestPermissions(need.toTypedArray(), 42)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 42) {
            val cb = afterPermissions
            afterPermissions = null
            cb?.invoke()
        }
    }

    private fun onDownload() {
        withPermissions { ModelDownloadService.start(this) }
    }

    private fun onStartStop() {
        if (MayaBus.running) {
            MayaService.stop(this)
            return
        }
        if (!ModelManager.allReady(this)) {
            toast("Download the models first")
            return
        }
        withPermissions {
            if (hasMic()) MayaService.start(this) else toast("Maya needs the microphone permission to hear \u201CHey Maya\u201D")
        }
    }

    private fun onCalibrate() {
        withPermissions {
            if (hasMic()) startCalibration() else toast("The microphone permission is needed for calibration")
        }
    }

    private fun say(msg: String) {
        MayaBus.calibration = msg
        MayaBus.changed()
    }

    /**
     * Records "Hey Maya" up to three accepted times. No beep (it bled into the clip): a short vibration and the
     * on-screen prompt are the cue. Each clip is trimmed to the spoken part, decoded by the wake-word model's own
     * recogniser, and checked by the native quality gate; only accepted samples become trigger phrases.
     * A running Maya is paused for the duration and restarted afterwards, so the new phrases apply by themselves.
     */
    private fun startCalibration() {
        if (calibrating) return
        val wasRunning = MayaBus.running
        calibrating = true
        say(if (wasRunning) "Pausing Maya for a moment\u2026" else "Get ready\u2026")
        render()
        worker.execute {
            val found = ArrayList<String>()
            try {
                if (wasRunning) {
                    MayaService.stop(this)
                    val deadline = System.currentTimeMillis() + 20_000
                    while (MayaBus.running && System.currentTimeMillis() < deadline) Thread.sleep(100)
                    if (MayaBus.running) {
                        say("Maya did not stop in time. Stop it first, then calibrate.")
                        return@execute
                    }
                }
                val cfg = ModelManager.buildConfig(this, prefs, false)
                var good = 0
                var attempts = 0
                while (good < 3 && attempts < 8) {
                    attempts++
                    say("Get ready\u2026  (${good + 1} of 3)")
                    Thread.sleep(900)
                    Haptics.tick(this)
                    Thread.sleep(350)  // let the vibration motor settle before the microphone opens
                    say(UiText.calibrationPrompt(good + 1, 3))
                    val raw = Calibration.record(2.4f)
                    if (raw == null) {
                        say("Could not open the microphone.")
                        break
                    }
                    val clip = SampleTrimmer.trim(raw)
                    if (clip == null) {
                        say("I did not hear you clearly. Speak up a little and say just \u201CHey Maya\u201D.")
                        Thread.sleep(1600)
                        continue
                    }
                    say("Listening\u2026")
                    val res = try {
                        NativeBridge.nativeCalibrateDecode(cfg, clip.pcm)
                    } catch (t: Throwable) {
                        "ERR\t${t.message}"
                    }
                    val parts = res.split('\t')
                    if (parts[0] == "OK" && parts.size >= 3) {
                        good++
                        if (parts[2] !in found) found.add(parts[2])
                        say(UiText.calibrationHeard(PhraseBook.display(parts[2]), good, 3))
                    } else {
                        say(parts.getOrElse(1) { "Try again" })
                    }
                    Thread.sleep(1500)
                }
                if (found.isNotEmpty()) prefs.phrases = PhraseBook.merge(prefs.phrases, found)
                say(
                    when {
                        found.isEmpty() -> "No usable samples. Try again in a quieter spot, saying just \u201CHey Maya\u201D."
                        wasRunning -> "Saved. Restarting Maya with your pronunciation\u2026"
                        else -> "Saved. Tap Start Maya to use your pronunciation."
                    }
                )
            } finally {
                calibrating = false
                MayaBus.changed()
                runOnUiThread {
                    // the outcome line is only a status message: let it disappear after a while
                    window.decorView.postDelayed({
                        if (!calibrating) {
                            MayaBus.calibration = ""
                            MayaBus.changed()
                        }
                    }, 9000)
                    if (wasRunning) MayaService.start(this)  // visible activity: allowed to start the microphone service
                }
            }
        }
    }

    @Volatile private var recordingDataset = false

    /** One guided session: the user repeats "Hey Maya" for ~75 s; every utterance becomes a WAV clip. */
    private fun onRecordSession() {
        if (recordingDataset) return
        withPermissions {
            if (!hasMic()) {
                toast("The microphone permission is needed to record")
                return@withPermissions
            }
            if (MayaBus.running) {
                toast("Stop Maya first (the recorder needs the microphone)")
                return@withPermissions
            }
            recordingDataset = true
            MayaBus.dataset = "Get ready\u2026"
            render()
            worker.execute {
                var last = 0
                val session = WakeRecorder.recordSession(
                    this,
                    onClip = { index, _ ->
                        last = index
                        runOnUiThread {
                            Haptics.tick(this)
                            MayaBus.dataset = "Recording\u2026 $index clip(s). Keep saying \u201CHey Maya\u201D."
                            render()
                        }
                    },
                    onTick = { sec ->
                        if (sec % 15 == 0) runOnUiThread {
                            MayaBus.dataset = "Recording\u2026 ${WakeRecorder.SECONDS - sec} s left, $last clip(s)."
                            render()
                        }
                    },
                )
                recordingDataset = false
                val total = WakeRecorder.clipCount(this)
                MayaBus.dataset = when {
                    session == null -> "Could not open the microphone."
                    session.clips == 0 -> "No utterances found. Say \u201CHey Maya\u201D a bit louder, with a pause after each."
                    session.peak < 1500 -> "Saved ${session.clips} clips, but the level was low. Move closer to the phone next time."
                    else -> "Saved ${session.clips} clips. Total: $total."
                }
                if (session != null && session.clips > 0) prefs.datasetSessions = prefs.datasetSessions + 1
                runOnUiThread { render() }
            }
        }
    }

    private fun onExportDataset() {
        worker.execute {
            val path = WakeRecorder.exportZip(this)
            runOnUiThread {
                toast(
                    if (path == null) "Nothing to export yet"
                    else "Saved to $path \u2014 upload it to Kaggle or send it on."
                )
            }
        }
    }

    private fun onBattery() {
        val pm = getSystemService(PowerManager::class.java)
        val pkg = Uri.parse("package:$packageName")
        try {
            if (!pm.isIgnoringBatteryOptimizations(packageName)) {
                startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, pkg))
            } else {
                startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, pkg))
                toast("Open Battery and choose \u201CNo restrictions\u201D. On Xiaomi also enable Autostart.")
            }
        } catch (e: Exception) {
            startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, pkg))
        }
    }

    private fun confirmDelete() {
        AlertDialog.Builder(this)
            .setTitle("Delete downloaded models?")
            .setMessage("This frees about 1 GB. You can download them again at any time.")
            .setPositiveButton("Delete") { _, _ ->
                if (MayaBus.running) MayaService.stop(this)
                ModelManager.deleteAll(this)
                MayaBus.download = DownloadStatus()
                render()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }
}
