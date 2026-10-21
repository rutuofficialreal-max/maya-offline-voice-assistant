package com.maya.assistant

import android.app.Application
import android.content.Context
import android.os.Build
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Crash recorder. Nothing here changes how Maya works: if the process dies with an uncaught
 * exception, the full stack trace (+ what the start-up was doing) is written to a file, and the
 * next launch shows it on screen with Copy / Share buttons instead of just dying again.
 *
 * Files live in filesDir and in externalFilesDir (the latter is often reachable from a file
 * manager or USB, which helps when the app cannot even show its own UI).
 */
class MayaApp : Application() {
    override fun onCreate() {
        super.onCreate()
        rotateStartupLog(this)   // keep the previous run's breadcrumbs for the crash report
        installCrashHandler(this)
        stage(this, "app:onCreate")
    }

    companion object {
        private const val CRASH = "last-crash.txt"
        private const val STARTUP = "startup.log"
        private const val STARTUP_PREV = "startup.prev.log"
        private const val HEALTHY = "start:healthy"

        // ------------------------------------------------------------------ start-up breadcrumbs

        /** Records what the current start is doing, so a crash (even a native one) shows where it died. */
        fun stage(ctx: Context, what: String) {
            val line = time() + "  " + what + "\n"
            for (d in dirs(ctx)) {
                try {
                    File(d, STARTUP).appendText(line)
                } catch (_: Throwable) {
                }
            }
        }

        fun markHealthy(ctx: Context) = stage(ctx, HEALTHY)

        private fun rotateStartupLog(ctx: Context) {
            for (d in dirs(ctx)) {
                try {
                    val cur = File(d, STARTUP)
                    if (cur.isFile && cur.length() > 0) {
                        cur.copyTo(File(d, STARTUP_PREV), overwrite = true)
                        cur.delete()
                    }
                } catch (_: Throwable) {
                }
            }
        }

        fun startupLog(ctx: Context): String =
            dirs(ctx).asSequence().mapNotNull { d ->
                try {
                    File(d, STARTUP).takeIf { it.isFile }?.readText()
                } catch (_: Throwable) {
                    null
                }
            }.firstOrNull { !it.isNullOrBlank() } ?: ""

        /** The previous run's breadcrumbs, if that run did not finish starting up cleanly. */
        fun previousStartWasUnclean(ctx: Context): String? {
            val prev = dirs(ctx).asSequence().mapNotNull { d ->
                try {
                    File(d, STARTUP_PREV).takeIf { it.isFile }?.readText()
                } catch (_: Throwable) {
                    null
                }
            }.firstOrNull { !it.isNullOrBlank() } ?: return null
            if (prev.contains(HEALTHY)) return null
            return prev.trim().lines().lastOrNull { it.isNotBlank() }
        }

        // ------------------------------------------------------------------ crash handling

        fun installCrashHandler(ctx: Context) {
            val previous = Thread.getDefaultUncaughtExceptionHandler()
            Thread.setDefaultUncaughtExceptionHandler { thread, error ->
                try {
                    record(ctx, thread, error)
                } catch (_: Throwable) {
                }
                previous?.uncaughtException(thread, error)
            }
        }

        /** Writes [error] with device info and the start-up breadcrumbs. Called by the UI thread too. */
        fun record(ctx: Context, thread: Thread, error: Throwable) {
            val sw = StringWriter()
            val p = PrintWriter(sw)
            p.println("Maya crashed")
            p.println("when:    " + time())
            p.println("thread:  " + thread.name)
            p.println("device:  " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE +
                " (API " + Build.VERSION.SDK_INT + "), " + Build.SUPPORTED_ABIS.joinToString("/"))
            p.println("app:     " + versionName(ctx))
            p.println()
            p.println("--- start-up breadcrumbs ---")
            p.println(startupLog(ctx).ifBlank { "(none)" })
            p.println()
            p.println("--- stack trace ---")
            error.printStackTrace(p)
            p.flush()
            writeAll(ctx, CRASH, sw.toString())
        }

        /** Reads (and clears) the crash written by the previous run. */
        fun takeCrash(ctx: Context): String? {
            var text: String? = null
            for (d in dirs(ctx)) {
                val f = File(d, CRASH)
                try {
                    if (text == null && f.isFile && f.length() > 0) text = f.readText()
                    f.delete()
                } catch (_: Throwable) {
                }
            }
            return text
        }

        // ------------------------------------------------------------------ helpers

        private fun dirs(ctx: Context): List<File> {
            val out = ArrayList<File>(2)
            try {
                out.add(ctx.filesDir)
            } catch (_: Throwable) {
            }
            try {
                ctx.getExternalFilesDir(null)?.let { out.add(it) }
            } catch (_: Throwable) {
            }
            return out
        }

        private fun writeAll(ctx: Context, name: String, text: String) {
            for (d in dirs(ctx)) {
                try {
                    File(d, name).writeText(text)
                } catch (_: Throwable) {
                }
            }
        }

        private fun time(): String = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US).format(Date())

        private fun versionName(ctx: Context): String = try {
            val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
            (pi.versionName ?: "?") + " (" + pi.longVersionCode + ")"
        } catch (_: Throwable) {
            "?"
        }
    }
}
