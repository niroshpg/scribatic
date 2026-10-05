package com.scribatic.app.engine

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.Test

/**
 * docs/backup-format.md, checked against iOS: Backup.swift sealing this same
 * plaintext with this passphrase, salt, nonce prefix and iteration count wrote
 * a file with the SHA-256 below. Matching it byte for byte means a backup from
 * either platform opens on the other.
 */
class BackupTest {
    private val passphrase = "correct horse ✓ é"
    private val salt = ByteArray(16) { it.toByte() }
    private val prefix = ByteArray(8) { (100 + it).toByte() }
    private val plain = ByteArray((1 shl 20) * 5 / 2) { ((it * 31 + 7) and 0xFF).toByte() }

    private fun sealed(): ByteArray {
        val out = ByteArrayOutputStream()
        Backup.SealedWriter(out, passphrase, salt, prefix, 1000).apply {
            write(plain)
            finish()
        }
        return out.toByteArray()
    }

    @Test
    fun matchesTheIosVector() {
        val sha = MessageDigest.getInstance("SHA-256").digest(sealed()).joinToString("") { "%02x".format(it) }
        assertEquals("7500b1d8e7c7bfbdd3ce606d6baae6f2884aecc2c962e7abc9da0380be2aac16", sha)
    }

    @Test
    fun opensWithThePassphraseOnly() {
        val file = sealed()
        val reader = Backup.SealedReader(ByteArrayInputStream(file), passphrase)
        val back = ByteArrayOutputStream()
        while (true) back.write(reader.next() ?: break)
        assertArrayEquals(plain, back.toByteArray())

        try {
            Backup.SealedReader(ByteArrayInputStream(file), "wrong").next()
            fail("a wrong passphrase must not open it")
        } catch (e: Backup.WrongPassphrase) {
        }
    }

    @Test
    fun aTruncatedBackupFails() {
        val file = sealed()
        // Drop the last chunk: what is left must not pass for a whole backup.
        val lastLength = 524_288 + 16
        val cut = file.copyOf(file.size - 4 - lastLength)
        val reader = Backup.SealedReader(ByteArrayInputStream(cut), passphrase)
        try {
            while (reader.next() != null) {}
            fail("a truncated backup must not read as complete")
        } catch (e: Backup.Damaged) {
        }
    }

    @Test
    fun archiveRoundTrip() {
        val dir = kotlin.io.path.createTempDirectory("backup").toFile()
        val db = File(dir, "db.sqlite").apply { writeText("database bytes") }
        val recording = File(dir, "recording-1.wav").apply { writeBytes(plain.copyOf(1_500_000)) }
        val out = ByteArrayOutputStream()
        Backup.write(db, listOf(recording), out, "pass phrase") {}
        val unpacked = File(dir, "unpacked")
        Backup.unpack(ByteArrayInputStream(out.toByteArray()), out.size().toLong(), "pass phrase", unpacked) {}
        assertEquals("database bytes", File(unpacked, "notes.sqlite").readText())
        assertArrayEquals(plain.copyOf(1_500_000), File(unpacked, "recordings/recording-1.wav").readBytes())
        dir.deleteRecursively()
    }
}
