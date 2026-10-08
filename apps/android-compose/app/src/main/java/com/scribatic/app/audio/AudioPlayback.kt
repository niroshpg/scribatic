package com.scribatic.app.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Plays back a capture written by [AudioCapture], or the M4A it is kept as
 * once its note is finished (see [RecordingFiles]).
 *
 * The file is 16 kHz mono float32 WAV, so once past the header playback
 * streams it straight into an [AudioTrack] configured identically. Nothing to
 * negotiate — the format is fixed by the engine's input requirement, which is
 * the whole reason capture writes it this way.
 */
class AudioPlayback {

    private val playing = AtomicBoolean(false)
    private var track: AudioTrack? = null
    private var worker: Thread? = null

    val isPlaying: Boolean get() = playing.get()

    /**
     * The whole file, or [fromMs] to [toMs] of it: one card of a transcript.
     * [onFinished] is invoked when that runs out, or after [stop].
     */
    fun play(file: File, fromMs: Long = 0, toMs: Long? = null, onFinished: () -> Unit) {
        if (playing.get()) return
        if (!file.exists() || file.length() == 0L) {
            onFinished()
            return
        }

        val minBuffer = AudioTrack.getMinBufferSize(
            AudioCapture.SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_FLOAT,
        )
        val bufferBytes = maxOf(minBuffer, 4096 * 4)

        val output = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                    .setSampleRate(AudioCapture.SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build(),
            )
            .setBufferSizeInBytes(bufferBytes)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()

        track = output
        playing.set(true)
        output.play()

        worker = thread(name = "scribatic-playback") {
            if (file.extension.equals("m4a", ignoreCase = true)) {
                runCatching { streamCompressed(file, fromMs, toMs, output) }
                stopTrack()
                onFinished()
                return@thread
            }
            val frames = 4096
            val bytes = ByteArray(frames * 4)
            val floats = FloatArray(frames)
            // 4 bytes a sample: offsets stay on a sample, or it plays as noise.
            val bytesPerMs = AudioCapture.SAMPLE_RATE * 4L / 1000
            var remaining = if (toMs != null) maxOf(0L, toMs - fromMs) * bytesPerMs else Long.MAX_VALUE
            BufferedInputStream(FileInputStream(file)).use { input ->
                // Past the WAV header, or it plays as a click of noise.
                input.skip(WavFile.dataOffset(file) + maxOf(0L, fromMs) * bytesPerMs)
                while (playing.get() && remaining > 0) {
                    val read = input.read(bytes, 0, minOf(bytes.size.toLong(), remaining).toInt())
                    if (read <= 0) break
                    remaining -= read

                    val count = read / 4
                    ByteBuffer.wrap(bytes, 0, read).order(ByteOrder.LITTLE_ENDIAN)
                        .asFloatBuffer().get(floats, 0, count)
                    output.write(floats, 0, count, AudioTrack.WRITE_BLOCKING)
                }
            }
            stopTrack()
            onFinished()
        }
    }

    /**
     * A finished recording, kept as M4A (AAC): decoded with the platform's
     * decoder as it plays, from the AAC frame at or before [fromMs], the
     * samples before it dropped, until [toMs].
     */
    private fun streamCompressed(file: File, fromMs: Long, toMs: Long?, output: AudioTrack) {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            extractor.setDataSource(file.path)
            val track = (0 until extractor.trackCount).first {
                extractor.getTrackFormat(it).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            }
            extractor.selectTrack(track)
            val format = extractor.getTrackFormat(track)
            val decoder = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME)!!)
            codec = decoder
            decoder.configure(format, null, null, 0)
            decoder.start()

            val fromUs = maxOf(0L, fromMs) * 1000
            val toUs = toMs?.let { it * 1000 } ?: Long.MAX_VALUE
            extractor.seekTo(fromUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            var channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
            var floatOutput = false
            val info = MediaCodec.BufferInfo()
            var inputDone = false
            while (playing.get()) {
                if (!inputDone) {
                    val index = decoder.dequeueInputBuffer(10_000)
                    if (index >= 0) {
                        val size = extractor.readSampleData(decoder.getInputBuffer(index)!!, 0)
                        if (size < 0) {
                            decoder.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            decoder.queueInputBuffer(index, 0, size, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                val index = decoder.dequeueOutputBuffer(info, 10_000)
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    val changed = decoder.outputFormat
                    channels = changed.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    floatOutput = changed.containsKey(MediaFormat.KEY_PCM_ENCODING) &&
                        changed.getInteger(MediaFormat.KEY_PCM_ENCODING) == AudioFormat.ENCODING_PCM_FLOAT
                    continue
                }
                if (index < 0) continue
                if (info.presentationTimeUs >= toUs) {
                    decoder.releaseOutputBuffer(index, false)
                    break
                }
                val buffer = decoder.getOutputBuffer(index)!!.order(ByteOrder.LITTLE_ENDIAN)
                buffer.position(info.offset)
                buffer.limit(info.offset + info.size)
                val samples = if (floatOutput) {
                    FloatArray(buffer.remaining() / 4).also { buffer.asFloatBuffer().get(it) }
                } else {
                    val shorts = ShortArray(buffer.remaining() / 2).also { buffer.asShortBuffer().get(it) }
                    FloatArray(shorts.size) { shorts[it] / 32768f }
                }
                decoder.releaseOutputBuffer(index, false)
                // Mono, as recorded; a decoder that hands back stereo is averaged.
                val mono = if (channels <= 1) samples else FloatArray(samples.size / channels) { frame ->
                    var sum = 0f
                    for (c in 0 until channels) sum += samples[frame * channels + c]
                    sum / channels
                }
                // The frame the seek landed on starts before fromMs: drop the lead-in.
                val skip = if (info.presentationTimeUs < fromUs) {
                    ((fromUs - info.presentationTimeUs) * AudioCapture.SAMPLE_RATE / 1_000_000).toInt().coerceAtMost(mono.size)
                } else 0
                // And the frame that crosses toMs is cut there.
                val end = if (toUs != Long.MAX_VALUE) {
                    val left = ((toUs - info.presentationTimeUs) * AudioCapture.SAMPLE_RATE / 1_000_000).toInt()
                    left.coerceIn(skip, mono.size)
                } else mono.size
                if (end > skip) output.write(mono, skip, end - skip, AudioTrack.WRITE_BLOCKING)
                if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
            }
        } finally {
            runCatching { codec?.stop() }
            codec?.release()
            extractor.release()
        }
    }

    fun stop() {
        if (!playing.getAndSet(false)) return
        worker?.join(2_000)
        worker = null
    }

    private fun stopTrack() {
        playing.set(false)
        track?.run {
            if (state == AudioTrack.STATE_INITIALIZED) {
                if (playState == AudioTrack.PLAYSTATE_PLAYING) stop()
                release()
            }
        }
        track = null
    }
}
