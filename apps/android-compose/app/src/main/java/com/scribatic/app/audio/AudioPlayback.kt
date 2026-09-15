package com.scribatic.app.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Plays back a capture written by [AudioCapture].
 *
 * The file is raw 16 kHz mono float32 with no container, so playback streams it
 * straight into an [AudioTrack] configured identically. There is no header to
 * parse and nothing to negotiate — the format is fixed by the engine's input
 * requirement, which is the whole reason capture writes it this way.
 */
class AudioPlayback {

    private val playing = AtomicBoolean(false)
    private var track: AudioTrack? = null
    private var worker: Thread? = null

    val isPlaying: Boolean get() = playing.get()

    /** [onFinished] is invoked when the file runs out, or after [stop]. */
    fun play(file: File, onFinished: () -> Unit) {
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
            val frames = 4096
            val bytes = ByteArray(frames * 4)
            val floats = FloatArray(frames)
            BufferedInputStream(FileInputStream(file)).use { input ->
                while (playing.get()) {
                    val read = input.read(bytes)
                    if (read <= 0) break

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
