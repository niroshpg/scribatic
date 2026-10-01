package com.scribatic.app.engine

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.security.MessageDigest

/**
 * Brings model files into the app's private storage from files the user picked.
 *
 * The app has no network permission and downloads nothing. The user downloads
 * the files in a browser, then picks them here; each is copied into `filesDir`
 * and hashed on the way. A file is accepted only if its SHA-256 matches an
 * entry in the core's catalog, and it is stored under that entry's name —
 * whatever it was called when downloaded, and however many were picked at once.
 */
class ModelInstaller(private val context: Context, private val packs: PlayModelPacks) {

    /** Lazy, and read off the main thread: the first read loads the native engine. */
    val catalog: List<ModelSpec> by lazy { TranscriptionEngine.modelCatalog() }

    private val filesDir: File get() = context.filesDir
    private val prefs get() = context.getSharedPreferences("models", Context.MODE_PRIVATE)

    /**
     * Present with the catalog size. Size, not hash: hashing 1.4 GB on every
     * launch would cost seconds, and a file only gets here by passing the hash
     * check in [import].
     */
    fun isInstalled(model: ModelSpec): Boolean = fileFor(model) != null

    /**
     * Where the model is, if anywhere: the Play asset pack first (ADR-011),
     * then a file imported into filesDir. The engine maps whichever it gets,
     * so a pack-delivered model is never copied.
     */
    fun fileFor(model: ModelSpec): File? =
        packs.fileFor(model)
            ?: File(filesDir, model.fileName).takeIf { it.isFile && it.length() == model.sizeBytes }

    /**
     * Whether the user wants an optional model. Required ones always are; an
     * optional one only once the user asks for it — it is never downloaded
     * just because the app was installed. One already on the device (an
     * earlier build fetched it unasked) counts as wanted until switched off.
     */
    fun isWanted(model: ModelSpec): Boolean =
        model.required || prefs.getBoolean("want:${model.fileName}", isInstalled(model))

    fun setWanted(model: ModelSpec, wanted: Boolean) {
        prefs.edit().putBoolean("want:${model.fileName}", wanted).apply()
    }

    /**
     * The required models are present: the engine can start. An optional model
     * still downloading does not hold the app back; the engine picks it up the
     * next time it starts.
     */
    fun isReady(): Boolean = catalog.filter { it.required }.all(::isInstalled)

    sealed interface Result {
        data class Installed(val model: ModelSpec) : Result
        data class AlreadyInstalled(val model: ModelSpec) : Result
        data class NotAModel(val name: String) : Result
        data class Failed(val name: String, val reason: String) : Result
    }

    /**
     * Copies one picked file in, verifying it. [onProgress] gets 0..1. Runs on
     * the caller's thread: call it from a background dispatcher.
     */
    fun import(uri: Uri, onProgress: (String, Float) -> Unit): Result {
        val (name, size) = describe(uri)

        // Reject by size before copying: no catalog entry means no point
        // streaming a gigabyte to find out.
        val candidates = catalog.filter { size == null || it.sizeBytes == size }
        if (candidates.isEmpty()) return Result.NotAModel(name)
        candidates.singleOrNull()?.let { if (isInstalled(it)) return Result.AlreadyInstalled(it) }

        val partial = File(filesDir, ".import-${System.nanoTime()}.partial")
        try {
            val digest = MessageDigest.getInstance("SHA-256")
            val total = size ?: candidates.maxOf { it.sizeBytes }
            var copied = 0L
            val input = context.contentResolver.openInputStream(uri)
                ?: return Result.Failed(name, "the file could not be opened")
            input.use { stream ->
                partial.outputStream().buffered(1 shl 20).use { out ->
                    val buffer = ByteArray(1 shl 20)
                    var lastReported = -1
                    while (true) {
                        val read = stream.read(buffer)
                        if (read < 0) break
                        digest.update(buffer, 0, read)
                        out.write(buffer, 0, read)
                        copied += read
                        val percent = ((copied * 100) / total).toInt()
                        if (percent != lastReported) {
                            lastReported = percent
                            onProgress(name, percent / 100f)
                        }
                    }
                }
            }

            val hash = digest.digest().joinToString("") { "%02x".format(it) }
            val model = catalog.firstOrNull { it.sha256 == hash && it.sizeBytes == copied }
                ?: return Result.NotAModel(name)

            // Rename, not copy: same directory, so it is atomic and the model
            // path never holds a half-written file.
            val target = File(filesDir, model.fileName)
            if (!partial.renameTo(target)) return Result.Failed(name, "it could not be saved")
            return Result.Installed(model)
        } catch (t: Throwable) {
            return Result.Failed(name, t.message ?: "copy failed")
        } finally {
            partial.delete()
        }
    }

    /** Removes an optional model the user no longer wants, freeing its space. */
    fun remove(model: ModelSpec) {
        if (!model.required) File(filesDir, model.fileName).delete()
    }

    /** Leftovers of an import cut short by the process dying. */
    fun sweepPartials() {
        filesDir.listFiles { f -> f.name.startsWith(".import-") }?.forEach { it.delete() }
    }

    private fun describe(uri: Uri): Pair<String, Long?> {
        context.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)
                val name = if (nameIndex >= 0) cursor.getString(nameIndex) else null
                val size = if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) cursor.getLong(sizeIndex) else null
                return (name ?: uri.lastPathSegment ?: "file") to size
            }
        }
        return (uri.lastPathSegment ?: "file") to null
    }
}
