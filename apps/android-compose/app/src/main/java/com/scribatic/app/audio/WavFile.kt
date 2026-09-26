package com.scribatic.app.audio

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The recording format both apps share: WAV, mono, 16 kHz, 32-bit float,
 * little-endian — the engine's own input format. The core maps the file
 * straight into speaker diarization without converting it (ADR-009).
 *
 * The header is written with zero sizes when capture starts and patched when
 * it stops. A recording cut short by a crash keeps its zero sizes; the core
 * reads that as "everything that reached the disk", so nothing is lost.
 */
object WavFile {
    const val HEADER_BYTES = 44
    private const val FORMAT_IEEE_FLOAT: Short = 3

    fun header(sampleRate: Int, dataBytes: Int = 0): ByteArray =
        ByteBuffer.allocate(HEADER_BYTES).order(ByteOrder.LITTLE_ENDIAN).apply {
            put("RIFF".toByteArray()); putInt(if (dataBytes == 0) 0 else 36 + dataBytes)
            put("WAVE".toByteArray())
            put("fmt ".toByteArray()); putInt(16)
            putShort(FORMAT_IEEE_FLOAT); putShort(1)           // mono
            putInt(sampleRate); putInt(sampleRate * 4)         // byte rate
            putShort(4); putShort(32)                           // block align, bits
            put("data".toByteArray()); putInt(dataBytes)
        }.array()

    /** Writes the final sizes into a header written by [header]. */
    fun patchSizes(file: File) {
        RandomAccessFile(file, "rw").use { raf ->
            val data = (raf.length() - HEADER_BYTES).coerceAtLeast(0).toInt()
            raf.seek(0)
            raf.write(header(AudioCapture.SAMPLE_RATE, data))
        }
    }

    /**
     * Offset of the first sample, by walking the chunk list. Files this app
     * writes always answer 44; walking keeps playback honest about any other.
     */
    fun dataOffset(file: File): Long {
        RandomAccessFile(file, "r").use { raf ->
            val chunk = ByteArray(8)
            var pos = 12L
            while (pos + 8 <= raf.length()) {
                raf.seek(pos)
                raf.readFully(chunk)
                val size = ByteBuffer.wrap(chunk, 4, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xFFFFFFFFL
                if (String(chunk, 0, 4) == "data") return pos + 8
                pos += 8 + size + (size and 1)
            }
        }
        return HEADER_BYTES.toLong()
    }
}
