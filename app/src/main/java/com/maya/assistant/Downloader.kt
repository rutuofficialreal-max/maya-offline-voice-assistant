package com.maya.assistant

import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.compressors.bzip2.BZip2CompressorInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

class DownloadCancelled : Exception("cancelled")

/** Resumable HTTP download, SHA-256 verification and safe tar.bz2 extraction. */
object Downloader {
    private const val UA = "Maya/1.0 (Android; offline voice assistant)"

    /** Follows redirects by hand so the Range header survives every hop (GitHub and Hugging Face both redirect to a CDN). */
    private fun open(url: String, from: Long): HttpURLConnection {
        var current = URL(url)
        for (hop in 0 until 8) {
            val c = current.openConnection() as HttpURLConnection
            c.instanceFollowRedirects = false
            c.connectTimeout = 20_000
            c.readTimeout = 30_000
            c.setRequestProperty("User-Agent", UA)
            c.setRequestProperty("Accept-Encoding", "identity")
            if (from > 0) c.setRequestProperty("Range", "bytes=$from-")
            val code = c.responseCode
            if (code in 300..399) {
                val location = c.getHeaderField("Location") ?: throw IOException("Redirect without Location header")
                c.disconnect()
                current = URL(current, location)
                continue
            }
            return c
        }
        throw IOException("Too many redirects")
    }

    /** Downloads [url] into [part], resuming whatever is already there. Returns once the file is complete. */
    fun fetch(url: String, part: File, cancelled: () -> Boolean, onProgress: (done: Long, total: Long) -> Unit) {
        var existing = if (part.exists()) part.length() else 0L
        var conn = open(url, existing)
        var code = conn.responseCode
        if (code == 416) {  // our partial file does not fit the resource any more: start over
            conn.disconnect()
            part.delete()
            existing = 0L
            conn = open(url, 0L)
            code = conn.responseCode
        }
        val append: Boolean
        val total: Long
        when (code) {
            200 -> { append = false; existing = 0L; total = conn.contentLengthLong }
            206 -> { append = true; total = existing + conn.contentLengthLong }
            else -> { conn.disconnect(); throw IOException("Server answered HTTP $code") }
        }
        try {
            conn.inputStream.use { input ->
                FileOutputStream(part, append).use { out ->
                    val buf = ByteArray(128 * 1024)
                    var done = existing
                    var lastReport = 0L
                    while (true) {
                        if (cancelled()) throw DownloadCancelled()
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        val now = System.nanoTime()
                        if (now - lastReport > 250_000_000L) {
                            lastReport = now
                            onProgress(done, total)
                        }
                    }
                    onProgress(done, total)
                }
            }
        } finally {
            conn.disconnect()
        }
        if (total > 0 && part.length() != total) {
            throw IOException("Connection dropped (${part.length()} of $total bytes received)")
        }
    }

    fun sha256(file: File, cancelled: () -> Boolean, onProgress: (Long) -> Unit): String {
        val md = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buf = ByteArray(1 shl 20)
            var total = 0L
            while (true) {
                if (cancelled()) throw DownloadCancelled()
                val n = input.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
                total += n
                onProgress(total)
            }
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    /**
     * Extracts [archive] into [destDir], dropping the archive's top-level folder and any test_wavs.
     * Extracts into a sibling ".tmp" folder first and renames, so a half-extracted model is never used.
     */
    fun extractTarBz2(archive: File, destDir: File, cancelled: () -> Boolean) {
        val tmp = File(destDir.parentFile, destDir.name + ".tmp")
        tmp.deleteRecursively()
        if (!tmp.mkdirs()) throw IOException("Cannot create ${tmp.path}")
        val root = tmp.canonicalPath + File.separator
        BZip2CompressorInputStream(BufferedInputStream(FileInputStream(archive), 1 shl 16)).use { bz ->
            TarArchiveInputStream(bz).use { tar ->
                val buf = ByteArray(64 * 1024)
                var entry = tar.nextEntry as TarArchiveEntry?
                while (entry != null) {
                    if (cancelled()) throw DownloadCancelled()
                    val name = entry.name.removePrefix("./")
                    val inner = name.substringAfter('/', "")
                    if (inner.isNotEmpty() && !inner.contains("test_wavs") && !entry.isSymbolicLink) {
                        val out = File(tmp, inner)
                        if (!out.canonicalPath.startsWith(root)) throw IOException("Unsafe path in archive: $name")
                        if (entry.isDirectory) {
                            out.mkdirs()
                        } else {
                            out.parentFile?.mkdirs()
                            FileOutputStream(out).use { o ->
                                while (true) {
                                    val n = tar.read(buf)
                                    if (n < 0) break
                                    o.write(buf, 0, n)
                                }
                            }
                        }
                    }
                    entry = tar.nextEntry as TarArchiveEntry?
                }
            }
        }
        File(tmp, ".complete").writeText("ok")
        destDir.deleteRecursively()
        if (!tmp.renameTo(destDir)) throw IOException("Could not move the extracted files into place")
    }
}
