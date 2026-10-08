package com.scribatic.app.engine

import java.io.DataInputStream
import java.io.EOFException
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.security.SecureRandom
import java.text.Normalizer
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.PBEKeySpec
import javax.crypto.spec.SecretKeySpec

/**
 * Encrypted backup: every note and, if wanted, its recording, in one file the
 * user saves wherever they choose. The format is docs/backup-format.md, the
 * same byte for byte as iOS's Backup.swift, so a backup restores on either.
 *
 * Nothing here touches a network. The user picks the place; if that place is a
 * cloud folder, the file is unreadable there without their passphrase.
 */
object Backup {
    const val FILE_EXTENSION = "scribatic-backup"
    const val ITERATIONS = 600_000
    const val CHUNK_SIZE = 1 shl 20
    const val MINIMUM_PASSPHRASE = 8

    open class BackupException(message: String) : IOException(message)
    class NotABackup : BackupException("This isn't a Scribatic backup.")
    class WrongPassphrase : BackupException("The passphrase doesn't open this backup.")
    class Damaged : BackupException("The backup is damaged or incomplete, so it can't be restored.")

    // -- Writing ---------------------------------------------------------------------

    /** Packs [database] and [recordings] into [output], encrypted; [progress] gets 0..1. */
    fun write(database: File, recordings: List<File>, output: OutputStream, passphrase: String, progress: (Float) -> Unit) {
        val total = (listOf(database) + recordings).sumOf { it.length() }
        var done = 0L
        val sealed = SealedWriter(output, passphrase)
        sealed.write("SCRIBARC".toByteArray())
        val buffer = ByteArray(CHUNK_SIZE)
        for ((name, file) in listOf("notes.sqlite" to database) + recordings.map { "recordings/${it.name}" to it }) {
            val nameBytes = name.toByteArray(Charsets.UTF_8)
            sealed.write(ByteBuffer.allocate(2).putShort(nameBytes.size.toShort()).array())
            sealed.write(nameBytes)
            sealed.write(ByteBuffer.allocate(8).putLong(file.length()).array())
            FileInputStream(file).use { input ->
                while (true) {
                    val read = input.read(buffer)
                    if (read <= 0) break
                    sealed.write(buffer, 0, read)
                    done += read
                    progress(if (total > 0) done.toFloat() / total else 1f)
                }
            }
        }
        sealed.write(ByteArray(2))
        sealed.finish()
    }

    // -- Reading ---------------------------------------------------------------------

    /**
     * Decrypts and unpacks [input] into [folder]: `notes.sqlite` and
     * `recordings/`. Throws [WrongPassphrase] before writing anything if the
     * passphrase is wrong. [size] is the backup's length, for [progress].
     */
    fun unpack(input: InputStream, size: Long, passphrase: String, folder: File, progress: (Float) -> Unit) {
        val reader = SealedReader(input, passphrase)
        val archive = DataInputStream(ArchiveStream(reader))
        val magic = ByteArray(8).also { archive.readFully(it) }
        if (String(magic) != "SCRIBARC") throw Damaged()
        val recordings = File(folder, "recordings").apply { mkdirs() }
        val buffer = ByteArray(CHUNK_SIZE)
        try {
            while (true) {
                val nameLength = archive.readUnsignedShort()
                if (nameLength == 0) break
                val name = String(ByteArray(nameLength).also { archive.readFully(it) }, Charsets.UTF_8)
                var remaining = archive.readLong()
                // Only what a backup holds, and never outside the folder.
                val file = name.substringAfterLast('/')
                val target = when {
                    name == "notes.sqlite" -> File(folder, "notes.sqlite")
                    name == "recordings/$file" && file.startsWith("recording-") &&
                        (file.endsWith(".wav") || file.endsWith(".m4a")) -> File(recordings, file)
                    else -> throw Damaged()
                }
                FileOutputStream(target).use { out ->
                    while (remaining > 0) {
                        val read = archive.read(buffer, 0, minOf(remaining, buffer.size.toLong()).toInt())
                        if (read < 0) throw Damaged()
                        out.write(buffer, 0, read)
                        remaining -= read
                        progress(if (size > 0) reader.consumed.toFloat() / size else 1f)
                    }
                }
            }
        } catch (e: EOFException) {
            throw Damaged()
        }
    }

    // -- Envelope --------------------------------------------------------------------

    fun deriveKey(passphrase: String, salt: ByteArray, iterations: Int): SecretKeySpec {
        val normalised = Normalizer.normalize(passphrase, Normalizer.Form.NFC)
        val spec = PBEKeySpec(normalised.toCharArray(), salt, iterations, 256)
        val bytes = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).encoded
        spec.clearPassword()
        return SecretKeySpec(bytes, "AES")
    }

    fun header(salt: ByteArray, prefix: ByteArray, iterations: Int): ByteArray =
        ByteBuffer.allocate(48)
            .put("SCRIBKUP".toByteArray())
            .put(byteArrayOf(1, 0, 0, 0))
            .putInt(iterations)
            .put(salt)
            .put(prefix)
            .array()

    private fun nonce(prefix: ByteArray, index: Int): ByteArray = ByteBuffer.allocate(12).put(prefix).putInt(index).array()

    /**
     * Encrypts in 1 MiB chunks. A full chunk is held back until more data
     * arrives, so the last one written is always the one marked last.
     */
    class SealedWriter(
        private val output: OutputStream,
        passphrase: String,
        salt: ByteArray = ByteArray(16).also { SecureRandom().nextBytes(it) },
        private val prefix: ByteArray = ByteArray(8).also { SecureRandom().nextBytes(it) },
        iterations: Int = ITERATIONS,
    ) {
        private val header = header(salt, prefix, iterations)
        private val key = deriveKey(passphrase, salt, iterations)
        private val buffer = ByteArray(CHUNK_SIZE)
        private var filled = 0
        private var index = 0

        init {
            output.write(header)
        }

        fun write(data: ByteArray, offset: Int = 0, length: Int = data.size) {
            var at = offset
            val end = offset + length
            while (at < end) {
                if (filled == CHUNK_SIZE) {
                    seal(last = false)
                }
                val n = minOf(end - at, CHUNK_SIZE - filled)
                System.arraycopy(data, at, buffer, filled, n)
                filled += n
                at += n
            }
        }

        fun finish() {
            seal(last = true)
            output.flush()
        }

        private fun seal(last: Boolean) {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE, key, GCMParameterSpec(128, nonce(prefix, index)))
            cipher.updateAAD(header)
            cipher.updateAAD(byteArrayOf(if (last) 1 else 0))
            val sealed = cipher.doFinal(buffer, 0, filled)
            output.write(ByteBuffer.allocate(4).putInt(sealed.size).array())
            output.write(sealed)
            filled = 0
            index++
        }
    }

    /** Decrypts chunk by chunk, reading one length ahead to know which chunk is the last. */
    class SealedReader(input: InputStream, passphrase: String) {
        private val input = DataInputStream(input)
        private val header = ByteArray(48)
        private val prefix: ByteArray
        private val key: SecretKeySpec
        private var index = 0
        private var nextLength: Int?
        private var finished = false
        var consumed = 0L
            private set

        init {
            try {
                this.input.readFully(header)
            } catch (e: EOFException) {
                throw NotABackup()
            }
            if (String(header, 0, 8) != "SCRIBKUP" || header[8] != 1.toByte()) throw NotABackup()
            val fields = ByteBuffer.wrap(header)
            val iterations = fields.getInt(12)
            prefix = header.copyOfRange(32, 40)
            key = deriveKey(passphrase, header.copyOfRange(16, 32), iterations)
            consumed = 48
            nextLength = readLength()
        }

        /** The next chunk's plaintext, or null after the last. */
        fun next(): ByteArray? {
            if (finished) return null
            val length = nextLength ?: throw Damaged()
            if (length < 16 || length > CHUNK_SIZE + 16) throw Damaged()
            val sealed = ByteArray(length)
            try {
                input.readFully(sealed)
            } catch (e: EOFException) {
                throw Damaged()
            }
            consumed += 4L + length
            nextLength = readLength()
            val last = nextLength == null
            return try {
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, nonce(prefix, index)))
                cipher.updateAAD(header)
                cipher.updateAAD(byteArrayOf(if (last) 1 else 0))
                cipher.doFinal(sealed).also {
                    index++
                    finished = last
                }
            } catch (e: AEADBadTagException) {
                // The first chunk failing is the passphrase; a later one, damage.
                throw if (index == 0) WrongPassphrase() else Damaged()
            }
        }

        private fun readLength(): Int? {
            val bytes = ByteArray(4)
            val first = input.read(bytes, 0, 4)
            if (first <= 0) return null
            var read = first
            while (read < 4) {
                val n = input.read(bytes, read, 4 - read)
                if (n < 0) throw Damaged()
                read += n
            }
            return ByteBuffer.wrap(bytes).int
        }
    }

    /** The archive's bytes across chunk boundaries. */
    private class ArchiveStream(private val reader: SealedReader) : InputStream() {
        private var chunk = ByteArray(0)
        private var position = 0

        override fun read(): Int {
            if (!fill()) return -1
            return chunk[position++].toInt() and 0xFF
        }

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (len == 0) return 0
            if (!fill()) return -1
            val n = minOf(len, chunk.size - position)
            System.arraycopy(chunk, position, b, off, n)
            position += n
            return n
        }

        private fun fill(): Boolean {
            while (position >= chunk.size) {
                chunk = reader.next() ?: return false
                position = 0
            }
            return true
        }
    }
}
