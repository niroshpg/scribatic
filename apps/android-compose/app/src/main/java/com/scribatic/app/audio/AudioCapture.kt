package com.scribatic.app.audio

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Microphone capture for the engine.
 *
 * Records at exactly the rate the core expects — 16 kHz mono float — so there
 * is no resampling stage to get wrong. `AudioRecord` is asked for
 * `ENCODING_PCM_FLOAT` directly rather than reading 16-bit and converting,
 * which keeps the read loop a copy and nothing more.
 *
 * The read loop runs on its own thread, not a coroutine dispatcher. It is a
 * blocking realtime consumer: parking it on a shared pool thread would let an
 * unrelated suspending task delay a read and drop audio on the floor.
 */
class AudioCapture(private val outputFile: File) {

    companion object {
        const val SAMPLE_RATE = 16_000

        /** Matches the engine's own framing; small enough to keep latency low. */
        private const val FRAMES_PER_READ = 1600      // 100 ms
    }

    private val running = AtomicBoolean(false)
    private val paused = AtomicBoolean(false)

    private var record: AudioRecord? = null
    private var worker: Thread? = null

    val isRunning: Boolean get() = running.get()

    /**
     * Starts capture. [sink] is called from the capture thread with a buffer of
     * float samples; it must hand them straight to the engine and return.
     *
     * The caller is responsible for having RECORD_AUDIO granted — this throws
     * if it is not, rather than silently recording silence.
     */
    @SuppressLint("MissingPermission")
    fun start(sink: (FloatArray, Int) -> Unit) {
        if (running.get()) return

        injectedSource?.let { source ->
            startInjected(source, sink)
            return
        }

        val minBuffer = AudioRecord.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_FLOAT,
        )
        require(minBuffer > 0) { "AudioRecord reports no usable buffer size at 16 kHz mono float" }

        // Four read-sized buffers of slack. The minimum is the point at which
        // overrun begins, not a reasonable working size.
        val bufferBytes = maxOf(minBuffer, FRAMES_PER_READ * 4 * 4)

        val recorder = AudioRecord(
            MediaRecorder.AudioSource.VOICE_RECOGNITION,
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_FLOAT,
            bufferBytes,
        )
        check(recorder.state == AudioRecord.STATE_INITIALIZED) {
            "AudioRecord failed to initialise — is RECORD_AUDIO granted?"
        }

        record = recorder
        running.set(true)
        paused.set(false)
        recorder.startRecording()

        worker = thread(name = "scribatic-capture", priority = Thread.MAX_PRIORITY) {
            val buffer = FloatArray(FRAMES_PER_READ)
            // WAV around the same 16 kHz mono float32 samples. The container is
            // what lets the shared core read the file for speaker diarization
            // exactly as it reads the iOS app's, with one parser for both.
            BufferedOutputStream(FileOutputStream(outputFile)).use { out ->
                out.write(WavFile.header(SAMPLE_RATE))
                val bytes = ByteBuffer.allocate(FRAMES_PER_READ * 4).order(ByteOrder.LITTLE_ENDIAN)
                while (running.get()) {
                    val read = recorder.read(buffer, 0, buffer.size, AudioRecord.READ_BLOCKING)
                    if (read <= 0) continue
                    if (paused.get()) continue

                    sink(buffer, read)

                    bytes.clear()
                    for (i in 0 until read) bytes.putFloat(buffer[i])
                    out.write(bytes.array(), 0, read * 4)
                }
                out.flush()
            }
        }
    }

    /**
     * Keeps the recorder and the thread alive but stops consuming frames, so
     * resuming continues one recording rather than starting a second.
     */
    /**
     * Debug builds only: a WAV file standing in for the microphone, played into
     * the pipeline at real-time pace. An emulator has no conversation to hear,
     * and speaker identification cannot be exercised by one person talking at
     * a laptop. Everything downstream — engine, recording file, save,
     * diarization — runs exactly as it does for a real recording.
     */
    var injectedSource: File? = null

    private fun startInjected(source: File, sink: (FloatArray, Int) -> Unit) {
        running.set(true)
        paused.set(false)
        worker = thread(name = "scribatic-injected-audio") {
            val samples = readPcm16Mono(source)
            val buffer = FloatArray(FRAMES_PER_READ)
            val bytes = ByteBuffer.allocate(FRAMES_PER_READ * 4).order(ByteOrder.LITTLE_ENDIAN)
            var offset = 0
            var next = System.nanoTime()
            BufferedOutputStream(FileOutputStream(outputFile)).use { out ->
                out.write(WavFile.header(SAMPLE_RATE))
                while (running.get()) {
                    next += 100_000_000L
                    val wait = (next - System.nanoTime()) / 1_000_000L
                    if (wait > 0) Thread.sleep(wait)
                    if (paused.get() || offset >= samples.size) continue
                    val read = minOf(FRAMES_PER_READ, samples.size - offset)
                    System.arraycopy(samples, offset, buffer, 0, read)
                    offset += read
                    sink(buffer, read)
                    bytes.clear()
                    for (i in 0 until read) bytes.putFloat(buffer[i])
                    out.write(bytes.array(), 0, read * 4)
                }
                out.flush()
            }
        }
    }

    /** 16-bit PCM mono 16 kHz, the format the test conversations are in. */
    private fun readPcm16Mono(file: File): FloatArray {
        val offset = WavFile.dataOffset(file)
        val raw = file.readBytes()
        val count = ((raw.size - offset) / 2).toInt()
        val shorts = ByteBuffer.wrap(raw, offset.toInt(), count * 2).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer()
        return FloatArray(count) { shorts.get(it) / 32768f }
    }

    fun pause() {
        paused.set(true)
    }

    fun resume() {
        paused.set(false)
    }

    fun stop() {
        if (!running.getAndSet(false)) return
        worker?.join(2_000)
        worker = null
        record?.run {
            if (state == AudioRecord.STATE_INITIALIZED) stop()
            release()
        }
        record = null
        WavFile.patchSizes(outputFile)
    }
}
