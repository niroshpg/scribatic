package com.scribatic.app.audio

import android.content.Context
import android.net.Uri
import com.scribatic.app.share.ShareFiles
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import kotlin.math.abs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * How recordings are kept. A recording is made as WAV (16 kHz mono float32,
 * what the engine reads, and recoverable if the app dies mid-recording) and,
 * once its note is finished, kept as M4A: AAC at 48 kbit/s, about 21 MB an
 * hour instead of 230. The engine does not decode M4A, so improving a
 * compressed note's transcript or identifying its speakers again decodes it
 * to a temporary WAV first.
 */
object RecordingFiles {

    fun isCompressed(file: File): Boolean = file.extension.equals("m4a", ignoreCase = true)

    /**
     * Compresses [wav] to an M4A of the same name beside it. Written in the
     * cache and moved into place, so a half-written file never sits among
     * the recordings. Blocking: seconds per hour of audio.
     */
    fun compress(context: Context, wav: File): File {
        val temporary = File(context.cacheDir, "compress-${UUID.randomUUID()}.m4a")
        try {
            ShareFiles.encodeM4a(wav, temporary, gain = levelling(wav))
            val target = File(wav.parentFile, wav.nameWithoutExtension + ".m4a")
            target.delete()
            if (!temporary.renameTo(target)) {
                temporary.copyTo(target, overwrite = true)
            }
            return target
        } finally {
            temporary.delete()
        }
    }

    /**
     * The gain that brings [wav]'s peak to -1 dBFS, at most 40 dB. AAC at
     * this rate drops what it judges inaudible, and a quiet recording (a
     * phone across the room from a television, peaking at -30 dBFS) lost its
     * speech with it; raised first, it keeps it. Recognition does not depend
     * on level, and playback is easier to hear. Never lowers a loud one.
     */
    fun levelling(wav: File): Float {
        var peak = 0f
        RandomAccessFile(wav, "r").use { input ->
            input.seek(WavFile.dataOffset(wav))
            val bytes = ByteArray(64 * 1024)
            while (true) {
                val read = input.read(bytes)
                if (read <= 0) break
                val samples = ByteBuffer.wrap(bytes, 0, read / 4 * 4).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer()
                while (samples.hasRemaining()) peak = maxOf(peak, abs(samples.get()))
            }
        }
        if (peak <= 0f) return 1f
        return (0.89f / peak).coerceIn(1f, 100f)
    }

    /**
     * Runs [work] with the recording as the engine can read it: null for a
     * WAV (the engine reads the note's own file), else the M4A decoded to a
     * temporary WAV, deleted afterwards.
     */
    suspend fun <T> withEngineAudio(context: Context, recording: File?, work: suspend (File?) -> T): T {
        if (recording == null || !isCompressed(recording)) return work(null)
        val decoded = File(context.cacheDir, "decoded-${UUID.randomUUID()}.wav")
        try {
            withContext(Dispatchers.IO) { AudioImport.convert(context, Uri.fromFile(recording), decoded) {} }
            return work(decoded)
        } finally {
            decoded.delete()
        }
    }
}

/** How long finished recordings are kept; transcripts are always kept. */
enum class KeepRecordings(val label: String, val explanation: String) {
    ALWAYS(
        "Always",
        "Recordings are kept, compressed (about 21 MB an hour), until you delete them.",
    ),
    MONTH(
        "30 days",
        "A recording is deleted 30 days after it was made. Its transcript, speakers and summary stay.",
    ),
    UNTIL_TRANSCRIBED(
        "Until transcribed",
        "A recording is deleted once its transcript is finished. Its transcript, speakers and summary " +
            "stay, but it can't be played, improved or have its speakers identified again.",
    );

    companion object {
        private const val KEY = "keepRecordings"

        fun current(context: Context): KeepRecordings =
            context.getSharedPreferences("settings", Context.MODE_PRIVATE).getString(KEY, null)
                ?.let { name -> entries.firstOrNull { it.name == name } } ?: ALWAYS

        fun set(context: Context, value: KeepRecordings) {
            context.getSharedPreferences("settings", Context.MODE_PRIVATE).edit().putString(KEY, value.name).apply()
        }

        /** Unix seconds, 30 days ago. */
        fun monthAgo(): Long = System.currentTimeMillis() / 1000 - 30L * 24 * 3600
    }
}
