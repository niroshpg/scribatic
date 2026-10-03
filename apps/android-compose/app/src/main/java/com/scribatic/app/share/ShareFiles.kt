package com.scribatic.app.share

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.pdf.PdfDocument
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.text.Layout
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.StaticLayout
import android.text.TextPaint
import android.text.style.StyleSpan
import com.scribatic.app.audio.AudioCapture
import com.scribatic.app.audio.WavFile
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Files made for the share sheet: a transcript as a PDF, a recording as a
 * small M4A. Each is written to the cache's share/ folder (the only folder the
 * FileProvider exposes), which is emptied before every export, so at most one
 * shared file is ever left behind and the system may clear it at will.
 */
object ShareFiles {

    /** A fresh, empty share/ folder; `name` cleaned up to be a file name. */
    fun target(context: Context, name: String, extension: String): File {
        val dir = File(context.cacheDir, "share")
        dir.deleteRecursively()
        dir.mkdirs()
        val clean = name.replace(':', '.')                      // "2.40 am"
            .replace(Regex("[\\\\/*?\"<>|\\p{Cntrl}]+"), " ").trim().take(80)
        return File(dir, "${clean.ifEmpty { "Scribatic note" }}.$extension")
    }

    // -- PDF -----------------------------------------------------------------

    private const val PAGE_WIDTH = 595            // A4, in points
    private const val PAGE_HEIGHT = 842
    private const val MARGIN = 56

    /**
     * The exported transcript as an A4 document: the title and the date line
     * from [transcript]'s first two lines, then [summary] when there is one
     * (it brings its own headings, "Summary:" first, in the note's language),
     * then the transcript, each turn's "[01:23] Ana:" in bold. Speaker
     * prefixes are only bolded for [speakerNames], so a lecture sentence that
     * happens to contain a colon is left alone.
     */
    fun writePdf(file: File, transcript: String, summary: String, speakerNames: Collection<String>) {
        val lines = transcript.lines()
        val title = lines.firstOrNull().orEmpty()
        val meta = lines.getOrNull(1).orEmpty()
        val paragraphs = lines.drop(2).filter { it.isNotBlank() }

        val writer = PdfWriter()
        writer.text(title, size = 20f, bold = true)
        writer.text(meta, size = 10f, color = GREY, after = 18f)
        if (summary.isNotBlank()) {
            summary.lines().filter { it.isNotBlank() }.forEachIndexed { i, line ->
                val heading = line.trimEnd().endsWith(":") && !line.trimStart().startsWith("-")
                val text = line.trim().replace(Regex("^- "), "•  ")
                writer.text(text, size = if (heading) 12f else 11f, bold = heading, before = if (heading && i > 0) 8f else 0f, after = 3f)
            }
            writer.space(16f)
            writer.text("Transcript", size = 14f, bold = true, after = 6f)
        }
        val prefix = Regex("^(\\[[0-9:]+] )?(.*)$")
        paragraphs.forEach { paragraph ->
            val match = prefix.find(paragraph)!!
            val stamp = match.groupValues[1]
            val rest = match.groupValues[2]
            val speaker = speakerNames.firstOrNull { rest.startsWith("$it: ") }
                ?: Regex("^Speaker \\d+: ").find(rest)?.value?.removeSuffix(": ")
            val bold = stamp.length + (speaker?.let { it.length + 2 } ?: 0)
            val text = SpannableStringBuilder(paragraph).apply {
                if (bold > 0) setSpan(StyleSpan(Typeface.BOLD), 0, bold, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
            writer.text(text, size = 11f, after = 8f)
        }
        file.outputStream().use { writer.finish(it) }
    }

    private const val GREY = 0xFF6B6B6B.toInt()

    /** Lays text out top to bottom, a line at a time, starting pages as it fills them. */
    private class PdfWriter {
        private val document = PdfDocument()
        private var page: PdfDocument.Page? = null
        private var pageNumber = 0
        private var y = 0f
        private val width = PAGE_WIDTH - 2 * MARGIN
        private val bottom = PAGE_HEIGHT - MARGIN

        fun space(points: Float) { y += points }

        fun text(
            text: CharSequence,
            size: Float,
            bold: Boolean = false,
            color: Int = Color.BLACK,
            before: Float = 0f,
            after: Float = 4f,
        ) {
            if (text.isEmpty()) return
            val paint = TextPaint(TextPaint.ANTI_ALIAS_FLAG).apply {
                textSize = size
                this.color = color
                typeface = if (bold) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
            }
            val layout = StaticLayout.Builder.obtain(text, 0, text.length, paint, width)
                .setAlignment(Layout.Alignment.ALIGN_NORMAL)
                .setLineSpacing(0f, 1.15f)
                .build()
            y += before
            for (line in 0 until layout.lineCount) {
                val top = layout.getLineTop(line).toFloat()
                val height = layout.getLineBottom(line) - top
                if (page == null || y + height > bottom) newPage()
                val canvas = page!!.canvas
                canvas.save()
                canvas.translate(MARGIN.toFloat(), y - top)
                canvas.clipRect(0f, top, width.toFloat(), top + height)
                layout.draw(canvas)
                canvas.restore()
                y += height
            }
            y += after
        }

        private fun newPage() {
            page?.let { document.finishPage(it) }
            pageNumber += 1
            page = document.startPage(PdfDocument.PageInfo.Builder(PAGE_WIDTH, PAGE_HEIGHT, pageNumber).create()).also {
                val paint = TextPaint(TextPaint.ANTI_ALIAS_FLAG).apply { textSize = 9f; color = GREY }
                val label = "$pageNumber"
                it.canvas.drawText(label, (PAGE_WIDTH - paint.measureText(label)) / 2, PAGE_HEIGHT - MARGIN / 2f, paint)
            }
            y = MARGIN.toFloat()
        }

        fun finish(out: java.io.OutputStream) {
            if (page == null) newPage()
            document.finishPage(page!!)
            document.writeTo(out)
            document.close()
        }
    }

    // -- M4A -----------------------------------------------------------------

    /** Speech at 16 kHz mono needs little more: about 14 MB an hour. */
    private const val AAC_BIT_RATE = 32_000

    /**
     * Encodes one of the app's recordings (WAV, 32-bit float, mono, 16 kHz)
     * as AAC-LC in an M4A, with the encoder every Android device has.
     * [onProgress] gets 0..1. Blocking: a few seconds per hour of audio.
     */
    fun encodeM4a(wav: File, out: File, onProgress: (Float) -> Unit = {}) {
        val sampleRate = AudioCapture.SAMPLE_RATE
        val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, sampleRate, 1).apply {
            setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_BIT_RATE, AAC_BIT_RATE)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16 * 1024)
        }
        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
        val muxer = MediaMuxer(out.path, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        try {
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start()
            RandomAccessFile(wav, "r").use { input ->
                val start = WavFile.dataOffset(wav)
                val totalSamples = ((input.length() - start) / 4).coerceAtLeast(0)
                input.seek(start)
                val floats = ByteArray(16 * 1024)
                var samplesRead = 0L
                var inputDone = false
                var track = -1
                val info = MediaCodec.BufferInfo()
                while (true) {
                    if (!inputDone) {
                        val index = codec.dequeueInputBuffer(10_000)
                        if (index >= 0) {
                            val buffer = codec.getInputBuffer(index)!!
                            buffer.clear()
                            // 4 bytes of float in, 2 bytes of 16-bit PCM out.
                            val want = minOf(floats.size, buffer.remaining() * 2) / 4 * 4
                            val read = input.read(floats, 0, want)
                            val timeUs = samplesRead * 1_000_000 / sampleRate
                            if (read <= 0) {
                                codec.queueInputBuffer(index, 0, 0, timeUs, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                                inputDone = true
                            } else {
                                val samples = ByteBuffer.wrap(floats, 0, read).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer()
                                val pcm = buffer.order(ByteOrder.LITTLE_ENDIAN)
                                while (samples.hasRemaining()) {
                                    val s = (samples.get().coerceIn(-1f, 1f) * Short.MAX_VALUE).toInt()
                                    pcm.putShort(s.toShort())
                                }
                                codec.queueInputBuffer(index, 0, pcm.position(), timeUs, 0)
                                samplesRead += read / 4
                                if (totalSamples > 0) onProgress(samplesRead.toFloat() / totalSamples)
                            }
                        }
                    }
                    val outIndex = codec.dequeueOutputBuffer(info, 10_000)
                    when {
                        outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            track = muxer.addTrack(codec.outputFormat)
                            muxer.start()
                        }
                        outIndex >= 0 -> {
                            val data = codec.getOutputBuffer(outIndex)!!
                            val config = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
                            if (!config && info.size > 0 && track >= 0) {
                                data.position(info.offset).limit(info.offset + info.size)
                                muxer.writeSampleData(track, data, info)
                            }
                            codec.releaseOutputBuffer(outIndex, false)
                            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                        }
                    }
                }
                if (track >= 0) muxer.stop()
            }
        } finally {
            runCatching { codec.stop() }
            codec.release()
            runCatching { muxer.release() }
        }
    }
}
