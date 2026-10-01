package com.scribatic.app.engine

import android.content.Context
import com.google.android.play.core.assetpacks.AssetPackManagerFactory
import com.google.android.play.core.assetpacks.AssetPackState
import com.google.android.play.core.assetpacks.AssetPackStateUpdateListener
import com.google.android.play.core.assetpacks.model.AssetPackStatus
import com.google.android.play.core.ktx.packStates
import com.google.android.play.core.ktx.requestFetch
import com.google.android.play.core.ktx.requestPackStates
import com.google.android.play.core.ktx.requestRemovePack
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import java.io.File

/**
 * The models as Play asset packs (ADR-011).
 *
 * The Play Store downloads the packs; this app only asks for them and reads the
 * files where Play put them. There is no network code here and no INTERNET
 * permission anywhere in the app.
 *
 * Only an install that came from Play has packs. A debug build, a sideloaded
 * APK or an install on a device without the Play Store reports the API as
 * unavailable, and the setup screen falls back to importing the files.
 */
class PlayModelPacks(context: Context) {

    private val manager = AssetPackManagerFactory.getInstance(context)

    /** Which pack a model ships in. */
    fun packFor(model: ModelSpec): String = if (model.required) CORE else ANSWERS

    /** Where Play extracted a model's file, if its pack is on the device. */
    fun fileFor(model: ModelSpec): File? {
        val location = manager.getPackLocation(packFor(model)) ?: return null
        val path = location.assetsPath() ?: return null
        return File(path, model.fileName).takeIf { it.isFile && it.length() == model.sizeBytes }
    }

    /**
     * States of both packs, or null when this install cannot use Play asset
     * delivery at all — the signal to offer importing instead.
     */
    suspend fun states(): Map<String, AssetPackState>? {
        // Per pack: one pack Play doesn't know (a test bundle built without
        // the instruct pack) must not make the whole install look non-Play.
        // Only the core pack decides that.
        val core = runCatching { manager.requestPackStates(listOf(CORE)).packStates() }.getOrNull()
            ?: return null
        val answers = runCatching { manager.requestPackStates(listOf(ANSWERS)).packStates() }.getOrNull()
        return core + answers.orEmpty()
    }

    /** Asks Play to download packs. Progress arrives through [updates]. */
    suspend fun fetch(packs: List<String>) {
        // A refused request (unknown pack, Play unavailable mid-session) must
        // not take the app down; the setup screen still offers importing.
        // One request per pack, so a failure on the optional pack never holds
        // back the required one.
        for (pack in packs) runCatching { manager.requestFetch(listOf(pack)) }
    }

    /** Stops a download in progress. [remove] then frees whatever arrived. */
    fun cancel(pack: String) {
        runCatching { manager.cancel(listOf(pack)) }
    }

    /** Frees a pack's space; used when the instruct model is left out. */
    suspend fun remove(pack: String) {
        runCatching { manager.requestRemovePack(pack) }
    }

    /** Every state change of every pack, for as long as it is collected. */
    val updates: Flow<AssetPackState>
        get() = callbackFlow {
            val listener = AssetPackStateUpdateListener { state -> trySend(state) }
            manager.registerListener(listener)
            awaitClose { manager.unregisterListener(listener) }
        }

    /** Launches Play's own "download over mobile data?" prompt. */
    fun confirmCellular(launcher: androidx.activity.result.ActivityResultLauncher<androidx.activity.result.IntentSenderRequest>) {
        manager.showConfirmationDialog(launcher)
    }

    companion object {
        const val CORE = "models_core"
        const val ANSWERS = "models_answers"

        fun describe(state: AssetPackState): String? = when (state.status()) {
            AssetPackStatus.PENDING -> "Waiting for Google Play"
            AssetPackStatus.DOWNLOADING -> {
                val total = state.totalBytesToDownload()
                val percent = if (total > 0) state.bytesDownloaded() * 100 / total else 0
                "Downloading from Google Play — $percent%"
            }
            AssetPackStatus.TRANSFERRING -> "Installing — ${state.transferProgressPercentage()}%"
            AssetPackStatus.WAITING_FOR_WIFI -> "Waiting for Wi-Fi"
            AssetPackStatus.REQUIRES_USER_CONFIRMATION -> "Needs your confirmation in Google Play"
            AssetPackStatus.FAILED -> "Google Play couldn't download the models (error ${state.errorCode()})"
            else -> null
        }
    }
}
