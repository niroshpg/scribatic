package com.scribatic.app.ext

import android.content.Context
import androidx.compose.runtime.Composable
import com.scribatic.app.engine.NoteDetail
import com.scribatic.app.engine.TranscriptionEngine
import com.scribatic.app.ui.TranscriptUiState
import kotlinx.coroutines.flow.StateFlow
import java.io.File

/**
 * Add-ons: the paid features, which live in a private repository cloned into
 * pro/ at the root of this one. When it is there, Gradle compiles its sources
 * into the app and [install] finds its entry point; when it is not — any
 * public checkout — there are no add-ons and the app is complete without them.
 *
 * Nothing here knows what an add-on does. It gets a place on the note screen,
 * the engine, and the models, through [ExtensionHost].
 */
object Extensions {
    /** Sections add-ons put on the note screen, under the player. */
    val noteSections: MutableList<NoteSection> = mutableListOf()

    /**
     * Optional models an installed add-on uses, by file name. Only these are
     * offered in Models: without the add-on that uses it, a model would be a
     * download that does nothing.
     */
    val optionalModels: MutableSet<String> = mutableSetOf()

    private var installed = false

    /** Called once, from Application.onCreate. */
    fun install(context: Context) {
        if (installed) return
        installed = true
        // By name: this repository cannot refer to classes it does not contain.
        runCatching {
            Class.forName("com.scribatic.pro.ProExtensions")
                .getMethod("install", Context::class.java)
                .invoke(null, context)
        }
    }
}

/** A section on the note screen. */
interface NoteSection {
    @Composable
    fun Content(note: NoteDetail, host: ExtensionHost)
}

/** What an add-on can see of the app and ask of it. The view model implements it. */
interface ExtensionHost {
    val uiState: StateFlow<TranscriptUiState>

    /** The running engine, or null before it has started. */
    fun engine(): TranscriptionEngine?

    /** Where a model's file is, if it is on the device. */
    fun modelFile(fileName: String): File?

    /** Asks for an optional model; the Play Store downloads it. */
    fun requestModel(fileName: String)

    /** Re-reads a note, e.g. after an add-on changed it through the engine. */
    fun reloadNote(id: Long)

    /** A one-off message at the bottom of the screen. */
    fun showMessage(text: String)
}
