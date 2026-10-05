package com.scribatic.app.audio

import android.content.Context
import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import com.scribatic.app.engine.DeviceStatus
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.sin

/**
 * Audio recorded elsewhere (a voice memo, a call, a lecture), converted to what
 * the app records: 16 kHz mono float32 WAV, in the recordings directory, so the
 * transcript, speakers, playback and sharing all treat it exactly like a
 * recording made here.
 *
 * The platform's own decoders read the source (M4A/AAC, MP3, Opus, FLAC, WAV);
 * channels are averaged and the result resampled here. Nothing leaves the
 * device.
 */
object AudioImport {

    class ImportException(message: String) : IOException(message)

    /** Writes the audio at [source] to [destination]; [progress] gets 0..1. */
    fun convert(context: Context, source: Uri, destination: File, progress: (Float) -> Unit) {
        val extractor = MediaExtractor()
        try {
            extractor.setDataSource(context, source, null)
            val track = (0 until extractor.trackCount).firstOrNull {
                extractor.getTrackFormat(it).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            } ?: throw ImportException("This file has no audio that can be read. Try an M4A, MP3 or WAV file.")
            extractor.selectTrack(track)
            val format = extractor.getTrackFormat(track)
            val durationUs = if (format.containsKey(MediaFormat.KEY_DURATION)) format.getLong(MediaFormat.KEY_DURATION) else 0L

            val needed = durationUs / 1_000_000 * DeviceStatus.BYTES_PER_SECOND
            if (DeviceStatus.freeBytes(context) < needed + DeviceStatus.MINIMUM_FREE) {
                throw ImportException("There isn't enough free storage: it needs about ${needed / 1_000_000} MB.")
            }

            val codec = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME)!!)
            codec.configure(format, null, null, 0)
            codec.start()
            try {
                BufferedOutputStream(FileOutputStream(destination)).use { out ->
                    out.write(WavFile.header(AudioCapture.SAMPLE_RATE))
                    decode(extractor, codec, format, durationUs, out, progress)
                }
            } finally {
                codec.stop()
                codec.release()
            }
            WavFile.patchSizes(destination)
        } catch (e: Exception) {
            destination.delete()
            throw e as? IOException ?: ImportException("The audio could not be read: ${e.message}")
        } finally {
            extractor.release()
        }
    }

    private fun decode(
        extractor: MediaExtractor,
        codec: MediaCodec,
        input: MediaFormat,
        durationUs: Long,
        out: BufferedOutputStream,
        progress: (Float) -> Unit,
    ) {
        val info = MediaCodec.BufferInfo()
        var inputDone = false
        var resampler: Resampler? = null
        var channels = input.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
        var float = false
        val bytes = ByteBuffer.allocate(4 * 8192).order(ByteOrder.LITTLE_ENDIAN)
        var lastPercent = -1

        fun write(samples: FloatArray, count: Int) {
            var at = 0
            while (at < count) {
                bytes.clear()
                val n = minOf(count - at, bytes.capacity() / 4)
                for (i in 0 until n) bytes.putFloat(samples[at + i])
                out.write(bytes.array(), 0, n * 4)
                at += n
            }
        }

        while (true) {
            if (!inputDone) {
                val index = codec.dequeueInputBuffer(10_000)
                if (index >= 0) {
                    val buffer = codec.getInputBuffer(index)!!
                    val size = extractor.readSampleData(buffer, 0)
                    if (size < 0) {
                        codec.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                        inputDone = true
                    } else {
                        codec.queueInputBuffer(index, 0, size, extractor.sampleTime, 0)
                        extractor.advance()
                    }
                }
            }
            val index = codec.dequeueOutputBuffer(info, 10_000)
            when {
                index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val output = codec.outputFormat
                    channels = output.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    float = output.containsKey(MediaFormat.KEY_PCM_ENCODING) &&
                        output.getInteger(MediaFormat.KEY_PCM_ENCODING) == AudioFormat.ENCODING_PCM_FLOAT
                    resampler = Resampler(output.getInteger(MediaFormat.KEY_SAMPLE_RATE), AudioCapture.SAMPLE_RATE)
                }
                index >= 0 -> {
                    val buffer = codec.getOutputBuffer(index)!!.order(ByteOrder.LITTLE_ENDIAN)
                    buffer.position(info.offset)
                    buffer.limit(info.offset + info.size)
                    val frames = info.size / (channels * if (float) 4 else 2)
                    val mono = FloatArray(frames)
                    for (f in 0 until frames) {
                        var sum = 0f
                        for (c in 0 until channels) {
                            sum += if (float) buffer.float else buffer.short / 32768f
                        }
                        mono[f] = sum / channels
                    }
                    codec.releaseOutputBuffer(index, false)
                    val active = resampler ?: Resampler(input.getInteger(MediaFormat.KEY_SAMPLE_RATE), AudioCapture.SAMPLE_RATE)
                        .also { resampler = it }
                    val converted = active.process(mono, frames)
                    write(converted, converted.size)

                    if (durationUs > 0) {
                        val percent = (info.presentationTimeUs * 100 / durationUs).toInt().coerceIn(0, 100)
                        if (percent != lastPercent) {
                            lastPercent = percent
                            progress(percent / 100f)
                        }
                    }
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                }
            }
        }
    }

    /**
     * Streaming windowed-sinc resampler. Speech recognition is sensitive to
     * aliasing, so 44.1/48 kHz is not simply decimated: a low-pass at the
     * target's Nyquist frequency (a Blackman-windowed sinc) comes first.
     */
    private class Resampler(private val from: Int, private val to: Int) {
        private val step = from.toDouble() / to
        private val cutoff = minOf(1.0, to.toDouble() / from)
        private val halfWidth = 16
        private var history = FloatArray(0)
        /** Position of the next output sample in input samples, from history's start. */
        private var position = 0.0

        fun process(input: FloatArray, count: Int): FloatArray {
            if (from == to) return input.copyOf(count)
            val samples = FloatArray(history.size + count)
            history.copyInto(samples)
            input.copyInto(samples, history.size, 0, count)

            val reach = halfWidth / cutoff
            val output = FloatArray(maxOf(0, ((samples.size - position) / step).toInt() + 1))
            var produced = 0
            while (position + reach < samples.size && produced < output.size) {
                val centre = position
                val first = maxOf(0, floor(centre - reach).toInt() + 1)
                val last = minOf(samples.size - 1, floor(centre + reach).toInt())
                var sum = 0.0
                var weight = 0.0
                for (i in first..last) {
                    val x = (i - centre) * cutoff
                    val w = kernel(x)
                    sum += samples[i] * w
                    weight += w
                }
                output[produced++] = if (weight != 0.0) (sum / weight).toFloat() else 0f
                position += step
            }
            // Keep what the next call's first outputs still reach back to.
            val keepFrom = maxOf(0, floor(position - reach).toInt())
            history = samples.copyOfRange(keepFrom, samples.size)
            position -= keepFrom
            return output.copyOf(produced)
        }

        private fun kernel(x: Double): Double {
            if (x == 0.0) return 1.0
            if (kotlin.math.abs(x) >= halfWidth) return 0.0
            val sinc = sin(PI * x) / (PI * x)
            val n = (x + halfWidth) / (2 * halfWidth)
            val window = 0.42 - 0.5 * cos(2 * PI * n) + 0.08 * cos(4 * PI * n)
            return sinc * window
        }
    }
}
