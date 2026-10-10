package com.scribatic.app.engine

import android.content.Context
import java.io.File

/**
 * Finds the model files: in their Google Play asset packs (ADR-011), or in
 * `filesDir`, where a development build has them copied by hand (TESTING.md).
 * The app has no network permission; Play delivers the packs. Nothing else
 * offers the files, so there is no import.
 */
class ModelInstaller(private val context: Context, private val packs: PlayModelPacks) {

    /** Lazy, and read off the main thread: the first read loads the native engine. */
    val catalog: List<ModelSpec> by lazy { TranscriptionEngine.modelCatalog() }

    private val filesDir: File get() = context.filesDir
    private val prefs get() = context.getSharedPreferences("models", Context.MODE_PRIVATE)

    /**
     * Present with the catalog size. Size, not hash: hashing 1.4 GB on every
     * launch would cost seconds; a pack's files are checked by Play.
     */
    fun isInstalled(model: ModelSpec): Boolean = fileFor(model) != null

    /**
     * Where the model is, if anywhere: the Play asset pack first (ADR-011),
     * then a file imported into filesDir. The engine maps whichever it gets,
     * so a pack-delivered model is never copied.
     */
    fun fileFor(model: ModelSpec): File? {
        val first = locate(model) ?: return null
        val rest = partsOf(model)
        if (rest.isEmpty()) return first
        val others = rest.map { locate(it) ?: return null }
        if (others.all { it.parentFile == first.parentFile }) return first
        // Split across two Play packs, so in two directories; llama.cpp finds
        // the later parts beside the first, so they are linked side by side.
        val dir = File(filesDir, "linked").apply { mkdirs() }
        for ((spec, file) in listOf(model to first) + rest.zip(others)) {
            val link = File(dir, spec.fileName)
            if (link.canonicalPath != file.canonicalPath) {
                link.delete()
                android.system.Os.symlink(file.absolutePath, link.absolutePath)
            }
        }
        return File(dir, model.fileName)
    }

    /** The later parts of a model split across files; empty for most. */
    fun partsOf(model: ModelSpec): List<ModelSpec> = catalog.filter { it.partOf == model.fileName }

    /** One file of the catalog: in its Play pack, else imported into filesDir. */
    private fun locate(model: ModelSpec): File? =
        packs.fileFor(model)
            ?: File(filesDir, model.fileName).takeIf { it.isFile && it.length() == model.sizeBytes }

    /**
     * Whether the user wants an optional model. Required ones always are; an
     * optional one only once the user asks for it — it is never downloaded
     * just because the app was installed. One already on the device (an
     * earlier build fetched it unasked) counts as wanted until switched off.
     */
    fun isWanted(model: ModelSpec): Boolean {
        // A later part is wanted with its first part.
        if (model.partOf.isNotEmpty()) return isWanted(catalog.first { it.fileName == model.partOf })
        return model.required || prefs.getBoolean("want:${model.fileName}", isInstalled(model))
    }

    fun setWanted(model: ModelSpec, wanted: Boolean) {
        prefs.edit().putBoolean("want:${model.fileName}", wanted).apply()
    }

    /**
     * The required models are present: the engine can start. An optional model
     * still downloading does not hold the app back; the engine picks it up the
     * next time it starts.
     */
    fun isReady(): Boolean = catalog.filter { it.required }.all(::isInstalled)

    /** Removes an optional model the user no longer wants, freeing its space. */
    fun remove(model: ModelSpec) {
        if (model.required) return
        for (spec in listOf(model) + partsOf(model)) {
            File(filesDir, spec.fileName).delete()
            File(File(filesDir, "linked"), spec.fileName).delete()
        }
    }

    /** Leftovers of an import cut short by the process dying. */
    fun sweepPartials() {
        filesDir.listFiles { f -> f.name.startsWith(".import-") }?.forEach { it.delete() }
    }

}
