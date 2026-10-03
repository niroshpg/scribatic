package com.scribatic.app.ui

import android.app.Application
import android.content.Context
import android.text.format.DateFormat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.scribatic.app.BuildConfig
import com.scribatic.app.audio.AudioCapture
import com.scribatic.app.audio.AudioPlayback
import com.scribatic.app.engine.EngineConfig
import com.scribatic.app.engine.EngineStatus
import com.scribatic.app.engine.ModelInstaller
import com.scribatic.app.engine.ModelSpec
import com.scribatic.app.engine.PlayModelPacks
import com.google.android.play.core.assetpacks.model.AssetPackStatus
import android.net.Uri
import com.scribatic.app.engine.NoteDetail
import com.scribatic.app.engine.NoteSummary
import com.scribatic.app.engine.TranscriptSegment
import com.scribatic.app.engine.TranscriptionEngine
import com.scribatic.app.engine.TranscriptionService
import com.scribatic.app.ext.ExtensionHost
import com.scribatic.app.ext.Extensions
import com.scribatic.app.share.ShareFiles
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Date

/** What the recorder is doing, as the UI needs to understand it. */
enum class Phase { STARTING, READY, RECORDING, PAUSED, PROCESSING, FAILED }

/** Which screen is showing. The notes list is the root; the others sit over it. */
sealed interface Screen {
    /** First run, or any time a wanted model is missing; also reachable later. */
    data object Models : Screen
    /** Installing from files: download page + import. Its own screen, see ModelsScreen. */
    data object ModelFiles : Screen
    data object Notes : Screen
    data object Recorder : Screen
    data class Note(val id: Long) : Screen
}

/** One row of the model setup screen. */
data class ModelRow(val spec: ModelSpec, val installed: Boolean, val wanted: Boolean)

/** What the activity should hand to the system share sheet, once. */
sealed interface ShareRequest {
    val subject: String

    data class Text(override val subject: String, val text: String) : ShareRequest

    /** A file in the cache's share/ folder, handed over through the FileProvider. */
    data class Attachment(override val subject: String, val file: File, val mimeType: String) : ShareRequest
}

data class TranscriptUiState(
    val phase: Phase = Phase.STARTING,
    val step: String = "",
    val segments: List<TranscriptSegment> = emptyList(),
    val failure: String? = null,
    val screen: Screen = Screen.Notes,
    val notes: List<NoteSummary> = emptyList(),
    val note: NoteDetail? = null,
    val canIdentifySpeakers: Boolean = false,
    val playingNoteId: Long? = null,
    val busy: Boolean = false,
    /** What [busy] is doing, when it is worth saying: "Preparing the recording — 40%". */
    val busyStep: String? = null,
    val message: String? = null,
    val share: ShareRequest? = null,
    val models: List<ModelRow> = emptyList(),
    /** "Checking ggml-base.bin — 40%" while an import runs. */
    val importing: String? = null,
    /** This install came from Play, so the models arrive as asset packs. */
    val playDelivery: Boolean = false,
    /** "Downloading from Google Play — 40%", or null when idle. */
    val playStatus: String? = null,
    /** Play is holding a download for the user's OK (mobile data, large size). */
    val playNeedsConfirmation: Boolean = false,
    /** Whether Play has been asked yet; until then, don't offer a fallback. */
    val playChecked: Boolean = false,
    /** Play reported the required pack as failed or cancelled. */
    val playFailed: Boolean = false,
    /** The accurate model is on the device: notes are finished after Stop. */
    val canRefine: Boolean = false,
    /** The note being finished after Stop, what is happening and how far (-1: unknown). */
    val finishingNoteId: Long? = null,
    val finishingStep: String = "",
    val finishingProgress: Float = -1f,
    /** "auto" or the ISO 639-1 code recordings are pinned to. */
    val spokenLanguage: String = AUTO_LANGUAGE,
    /** The recording's language once known; empty while it is being detected. */
    val sessionLanguage: String = "",
) {
    /** The store can't provide the models: offer installing from files. */
    val storeUnavailable: Boolean get() = playChecked && (!playDelivery || playFailed)

    /** The required models are present, so the engine can start; see ModelInstaller.isReady. */
    val modelsReady: Boolean get() = models.isNotEmpty() && models.filter { it.spec.required }.all { it.installed }

    val label: String
        get() = when (phase) {
            Phase.STARTING   -> "Preparing"
            Phase.READY      -> "Ready"
            Phase.RECORDING  -> "Recording"
            Phase.PAUSED     -> "Paused"
            Phase.PROCESSING -> step
            Phase.FAILED     -> "Stopped"
        }
}

/**
 * Owns the engine for the lifetime of the app's single screen stack and
 * marshals every native call onto [Dispatchers.Default]. The main dispatcher
 * is touched only to publish an already-computed immutable [TranscriptUiState].
 *
 * This is an [AndroidViewModel] because the engine's paths are resolved against
 * `filesDir`, which needs a Context.
 */
class TranscriptionViewModel(application: Application) : AndroidViewModel(application), ExtensionHost {

    private val _uiState = MutableStateFlow(TranscriptUiState())
    override val uiState: StateFlow<TranscriptUiState> = _uiState.asStateFlow()

    private var engine: TranscriptionEngine? = null
    private var streamJob: Job? = null
    private var capture: AudioCapture? = null
    private var recordingFile: File? = null
    private var recordingStartedAt = 0L
    private val playback = AudioPlayback()
    private val packs = PlayModelPacks(application)
    private val installer = ModelInstaller(application, packs)
    private val settings = application.getSharedPreferences("settings", Context.MODE_PRIVATE)

    init {
        _uiState.update { it.copy(spokenLanguage = settings.getString("language", AUTO_LANGUAGE) ?: AUTO_LANGUAGE) }
    }

    fun setSpokenLanguage(code: String) {
        settings.edit().putString("language", code).apply()
        _uiState.update { it.copy(spokenLanguage = code) }
    }
    private var packUpdates: Job? = null
    private var preparing = false

    private val filesDir: File get() = getApplication<Application>().filesDir

    init {
        // "Stop and save" on the recording notification.
        viewModelScope.launch { TranscriptionService.stops.collect { stopRecording() } }
    }

    /** Inside filesDir, so covered by allowBackup=false and the extraction rules. */
    private val recordingsDir: File get() = File(filesDir, "recordings").apply { mkdirs() }

    /**
     * Warms the engine only. The microphone is deliberately NOT opened here: for
     * an app whose whole claim is that audio never leaves the device, a mic that
     * goes live the instant the app launches is the wrong default.
     */
    fun prepare() {
        if (engine != null || preparing) return
        preparing = true

        // Off the main thread from the first line: the model catalog comes from
        // the C++ core, so its first read loads libscribatic_engine.so and,
        // through it, ONNX Runtime and the ggml backends — tens of megabytes of
        // native code, which on the main thread was an ANR on first launch.
        viewModelScope.launch(Dispatchers.IO) {
            installer.sweepPartials()
            refreshModels()
            // Alongside, never before: binding to the Play Store's service can
            // take seconds, and the screen is decided by what is already on
            // disk. When a pack lands, its update moves the screen on.
            viewModelScope.launch(Dispatchers.IO) { startPlayDelivery() }
            if (!installer.isReady()) {
                preparing = false
                _uiState.update { it.copy(screen = Screen.Models) }
                return@launch
            }
            startEngine()
        }
    }

    private suspend fun startEngine() = withContext(Dispatchers.Default) {
        try {
            // From the Play pack if that is where a model is, else from filesDir.
            fun path(name: String): String {
                val model = installer.catalog.first { it.fileName == name }
                return (installer.fileFor(model) ?: File(filesDir, name)).absolutePath
            }
            val config = EngineConfig(
                whisperModelPath = path("ggml-base.bin"),
                llamaModelPath = path("insight-q4_k_m.gguf"),
                embedModelPath = path("embed-minilm-l6-v2.gguf"),
                databasePath = File(File(filesDir, "store").apply { mkdirs() }, "scribatic.sqlite").absolutePath,
                recordingsDirectory = recordingsDir.absolutePath,
                segmentationModelPath = path("speaker-segmentation.onnx"),
                speakerEmbeddingModelPath = path("speaker-embedding.onnx"),
                accurateModelPath = path("ggml-small-q8_0.bin"),
            )

            val created = TranscriptionEngine.create(config)
            if (created == null) {
                fail("Engine could not be created — are the model files in ${filesDir.absolutePath}?")
                return@withContext
            }

            val status = created.warmUp()
            if (status != EngineStatus.OK) {
                fail("Model load failed: $status")
                created.close()
                return@withContext
            }

            engine = created
            _uiState.update {
                it.copy(
                    phase = Phase.READY,
                    failure = null,
                    canIdentifySpeakers = created.canIdentifySpeakers(),
                    canRefine = created.canRefine(),
                    notes = created.listNotes(),
                )
            }
        } finally {
            preparing = false
        }
    }

    // -- Models -----------------------------------------------------------------------

    /**
     * On a Play install, asks for every wanted pack that is not already on the
     * device and follows their progress. On any other install, does nothing,
     * and the setup screen offers importing instead.
     */
    private suspend fun startPlayDelivery() {
        val states = packs.states()
        _uiState.update { it.copy(playDelivery = states != null, playChecked = true) }
        if (states == null) return

        if (packUpdates == null) {
            packUpdates = viewModelScope.launch(Dispatchers.IO) {
                packs.updates.collect { state ->
                    val waiting = state.status() == AssetPackStatus.WAITING_FOR_WIFI ||
                        state.status() == AssetPackStatus.REQUIRES_USER_CONFIRMATION
                    val failed = state.name() == PlayModelPacks.CORE &&
                        (state.status() == AssetPackStatus.FAILED || state.status() == AssetPackStatus.CANCELED)
                    _uiState.update {
                        it.copy(
                            playStatus = PlayModelPacks.describe(state),
                            playNeedsConfirmation = waiting,
                            playFailed = it.playFailed || failed,
                        )
                    }
                    if (state.status() == AssetPackStatus.COMPLETED) {
                        refreshModels()
                        // First run, blocked only on the download: carry on.
                        if (engine == null && installer.isReady()) continueFromModels()
                    }
                }
            }
        }

        val wanted = installer.catalog.filter(installer::isWanted).map(packs::packFor).distinct()
        val missing = wanted.filter { states[it]?.status() != AssetPackStatus.COMPLETED }
        packs.fetch(missing)
    }

    fun confirmPlayDownload(launcher: androidx.activity.result.ActivityResultLauncher<androidx.activity.result.IntentSenderRequest>) {
        packs.confirmCellular(launcher)
    }

    private fun refreshModels() {
        // An optional model is offered only when an add-on that uses it is installed.
        val rows = installer.catalog
            .filter { it.required || it.fileName in Extensions.optionalModels }
            .map { ModelRow(it, installer.isInstalled(it), installer.isWanted(it)) }
        _uiState.update { it.copy(models = rows) }
    }

    fun openModelFiles() = _uiState.update { it.copy(screen = Screen.ModelFiles) }

    fun openModels() {
        _uiState.update { it.copy(screen = Screen.Models) }
        viewModelScope.launch(Dispatchers.IO) { refreshModels() }
    }

    /**
     * Leaving out an optional model also deletes it if it is installed: the
     * only reason to decline the instruct model is its 1.2 GB.
     */
    fun setModelWanted(spec: ModelSpec, wanted: Boolean) {
        // Off the main thread: removing the instruct model deletes 1.2 GB.
        viewModelScope.launch(Dispatchers.IO) {
            installer.setWanted(spec, wanted)
            if (!wanted) installer.remove(spec)
            if (_uiState.value.playDelivery) {
                val pack = packs.packFor(spec)
                if (wanted) {
                    packs.fetch(listOf(pack))
                } else {
                    // Removing alone leaves a download that is under way running.
                    packs.cancel(pack)
                    packs.remove(pack)
                }
            }
            refreshModels()
        }
    }

    fun importModels(uris: List<Uri>) {
        if (uris.isEmpty() || _uiState.value.importing != null) return
        viewModelScope.launch(Dispatchers.IO) {
            val problems = mutableListOf<String>()
            for (uri in uris) {
                val result = installer.import(uri) { name, fraction ->
                    _uiState.update { it.copy(importing = "Checking $name — ${(fraction * 100).toInt()}%") }
                }
                when (result) {
                    is ModelInstaller.Result.NotAModel ->
                        problems += "${result.name} is not one of the model files."
                    is ModelInstaller.Result.Failed ->
                        problems += "${result.name}: ${result.reason}."
                    else -> Unit
                }
            }
            refreshModels()
            _uiState.update {
                it.copy(
                    importing = null,
                    message = problems.takeIf { p -> p.isNotEmpty() }?.joinToString("\n"),
                    // Everything needed is in: back to the setup screen, whose
                    // Continue is now enabled.
                    screen = if (it.modelsReady && it.screen == Screen.ModelFiles) Screen.Models else it.screen,
                )
            }
        }
    }

    /** From the setup screen: start the engine, or just return if it runs. */
    fun continueFromModels() {
        if (!_uiState.value.modelsReady) return
        _uiState.update { it.copy(screen = Screen.Notes) }
        prepare()
    }

    fun modelDownloadPage(): String = TranscriptionEngine.modelDownloadPage()

    // -- Navigation ---------------------------------------------------------------

    fun openRecorder() {
        if (_uiState.value.phase != Phase.READY) return
        _uiState.update { it.copy(screen = Screen.Recorder, segments = emptyList()) }
    }

    fun openNote(id: Long) {
        _uiState.update { it.copy(screen = Screen.Note(id), note = null) }
        reloadNote(id)
    }

    /** Back from any screen returns to the list; mid-recording, back is ignored. */
    fun back(): Boolean {
        val state = _uiState.value
        if (state.screen == Screen.Notes) return false
        if (state.screen == Screen.ModelFiles) {
            _uiState.update { it.copy(screen = Screen.Models) }
            return true
        }
        if (state.screen == Screen.Models) {
            // Nothing behind the setup screen until the engine is running.
            if (engine == null) return false
            _uiState.update { it.copy(screen = Screen.Notes) }
            return true
        }
        if (state.phase == Phase.RECORDING || state.phase == Phase.PAUSED || state.phase == Phase.PROCESSING) {
            return true
        }
        stopPlayback()
        _uiState.update { it.copy(screen = Screen.Notes, note = null) }
        return true
    }

    // -- Recording ----------------------------------------------------------------

    fun startRecording() {
        val engine = engine ?: return
        if (_uiState.value.phase != Phase.READY) return

        _uiState.update { it.copy(sessionLanguage = "") }
        viewModelScope.launch(Dispatchers.Default) {
            engine.setLanguage(_uiState.value.spokenLanguage)
            engine.beginSession()
            val file = File(recordingsDir, "recording-${System.currentTimeMillis()}.wav")
            val audio = AudioCapture(file)
            if (BuildConfig.DEBUG) {
                audio.injectedSource = File(filesDir, "inject/conversation.wav").takeIf { it.exists() }
            }
            try {
                audio.start { buffer, frameCount ->
                    // Capture thread. Lock-free on the C++ side, which is what
                    // makes calling straight through from here safe.
                    engine.pushAudio(buffer, frameCount)
                }
            } catch (t: Throwable) {
                fail(t.message ?: "Microphone could not be opened")
                return@launch
            }

            capture = audio
            recordingFile = file
            recordingStartedAt = System.currentTimeMillis()
            _uiState.update { it.copy(phase = Phase.RECORDING, failure = null, segments = emptyList()) }
            // Keeps the microphone available with the screen locked or another
            // app in front. Started now, while Record has just been tapped:
            // Android won't start a microphone service from the background.
            TranscriptionService.show(getApplication(), paused = false)

            streamJob?.cancel()
            streamJob = viewModelScope.launch(Dispatchers.Default) {
                engine.transcriptionStream().collect { batch ->
                    val language = engine.sessionLanguage()
                    _uiState.update { it.copy(segments = it.segments + batch, sessionLanguage = language) }
                }
            }
        }
    }

    fun pauseRecording() {
        if (_uiState.value.phase != Phase.RECORDING) return
        capture?.pause()
        _uiState.update { it.copy(phase = Phase.PAUSED) }
        TranscriptionService.show(getApplication(), paused = true)
    }

    fun resumeRecording() {
        if (_uiState.value.phase != Phase.PAUSED) return
        capture?.resume()
        _uiState.update { it.copy(phase = Phase.RECORDING) }
        TranscriptionService.show(getApplication(), paused = false)
    }

    /**
     * Stops capture and turns the recording into a saved note: decode the tail,
     * persist, identify speakers, open the note. Speakers are identified now,
     * while the audio certainly exists — deleting it may be the next thing the
     * user does.
     */
    fun stopRecording() {
        val phase = _uiState.value.phase
        if (phase != Phase.RECORDING && phase != Phase.PAUSED) return
        val engine = engine ?: return

        val stream = streamJob
        streamJob = null
        // The microphone closes now, but the service stays until the note is
        // saved, so Android does not kill a backgrounded app mid-save.
        processing("Finishing transcript")

        viewModelScope.launch(Dispatchers.Default) {
            capture?.stop()
            capture = null
            // The last pass must be over before the flush decodes: the engine
            // serialises calls, and this keeps the order deterministic too.
            stream?.cancelAndJoin()

            // The window is several seconds wide, so without this everything
            // said since the last boundary is never transcribed.
            val tail = engine.flush()
            if (tail.isNotEmpty()) _uiState.update { it.copy(segments = it.segments + tail) }

            processing("Saving")
            val startedAt = recordingStartedAt
            val title = DateFormat.getMediumDateFormat(getApplication()).format(Date(startedAt)) + ", " +
                DateFormat.getTimeFormat(getApplication()).format(Date(startedAt))
            val id = engine.saveSession(title, startedAt / 1000, recordingFile?.absolutePath)
            if (id == 0L) {
                fail("The recording could not be saved.")
                return@launch
            }

            // The preview is saved: show the note now, and finish it — the
            // accurate pass, then speakers — in the background.
            _uiState.update {
                it.copy(
                    phase = Phase.READY,
                    segments = emptyList(),
                    notes = engine.listNotes(),
                    screen = Screen.Note(id),
                )
            }
            reloadNote(id)
            finishNote(id)
        }
    }

    private var finishJob: Job? = null

    /**
     * The final transcript: the whole recording again with the accurate model,
     * then speakers from its words. Queued behind any note still finishing.
     * The "Saving…" notification stays up throughout, unless a new recording
     * has taken it over.
     */
    fun finishNote(id: Long) {
        val engine = engine ?: return
        val previous = finishJob
        lateinit var self: Job
        self = viewModelScope.launch(Dispatchers.Default) {
            previous?.join()
            val note = engine.loadNote(id) ?: return@launch
            var message: String? = null
            if (engine.canRefine() && note.audioPath != null) {
                finishing(id, "Improving transcript", 0f)
                val poll = launch {
                    while (true) {
                        delay(400)
                        val progress = engine.refineProgress()
                        _uiState.update { if (it.finishingNoteId == id) it.copy(finishingProgress = progress) else it }
                        if (!recordingNow()) TranscriptionService.saving(getApplication(), "Improving transcript — ${(progress * 100).toInt()}%")
                    }
                }
                val status = engine.refineTranscript(id)
                poll.cancel()
                if (status != EngineStatus.OK && status != EngineStatus.CANCELLED) message = "The transcript could not be improved: $status"
                reloadNote(id)
            }
            if (_uiState.value.canIdentifySpeakers && note.audioPath != null) {
                finishing(id, "Identifying speakers", -1f)
                val status = engine.identifySpeakers(id)
                if (status != EngineStatus.OK) message = "Speakers could not be identified: $status"
            }
            _uiState.update {
                it.copy(
                    finishingNoteId = null,
                    finishingStep = "",
                    notes = engine.listNotes(),
                    message = message ?: it.message,
                )
            }
            reloadNote(id)
            if (finishJob === self && !recordingNow()) TranscriptionService.hide(getApplication())
        }
        finishJob = self
    }

    private fun finishing(id: Long, step: String, progress: Float) {
        _uiState.update { it.copy(finishingNoteId = id, finishingStep = step, finishingProgress = progress) }
        if (!recordingNow()) TranscriptionService.saving(getApplication(), step)
    }

    private fun recordingNow() = _uiState.value.phase.let { it == Phase.RECORDING || it == Phase.PAUSED }

    // -- Correcting speakers ------------------------------------------------------

    /** Every segment of a block to [speaker]; a negative one makes a new speaker. */
    fun setBlockSpeaker(noteId: Long, segmentIds: List<Long>, speaker: Int) = noteOperation(noteId) { engine ->
        var target = speaker
        for ((index, segment) in segmentIds.withIndex()) {
            if (index == 0 && target < 0) {
                val status = engine.setSegmentSpeaker(noteId, segment, -1)
                if (status != EngineStatus.OK) return@noteOperation "Could not change the speaker: $status"
                target = engine.loadNote(noteId)?.segments?.firstOrNull { it.id == segment }?.speaker ?: return@noteOperation null
            } else {
                val status = engine.setSegmentSpeaker(noteId, segment, target)
                if (status != EngineStatus.OK) return@noteOperation "Could not change the speaker: $status"
            }
        }
        null
    }

    fun mergeSpeakers(noteId: Long, speakers: List<Int>, into: Int) = noteOperation(noteId) { engine ->
        val status = engine.mergeSpeakers(noteId, speakers, into)
        if (status != EngineStatus.OK) "Could not merge speakers: $status" else null
    }

    fun setLayout(noteId: Long, layout: String) = noteOperation(noteId) { engine ->
        val status = engine.setNoteLayout(noteId, layout)
        if (status != EngineStatus.OK) "Could not change the layout: $status" else null
    }

    // -- Notes ----------------------------------------------------------------------

    fun identifySpeakers(id: Long, expected: Int) = noteOperation(id) {
        if (_uiState.value.playingNoteId == id) stopPlayback()
        val status = it.identifySpeakers(id, expected)
        if (status != EngineStatus.OK) "Speakers could not be identified: $status" else null
    }

    fun renameSpeaker(id: Long, speaker: Int, name: String) = noteOperation(id) {
        val status = it.renameSpeaker(id, speaker, name.trim())
        if (status != EngineStatus.OK) "Could not rename: $status" else null
    }

    /** Removes the audio and keeps the transcript. */
    fun deleteRecording(id: Long) = noteOperation(id) {
        if (_uiState.value.playingNoteId == id) stopPlayback()
        val status = it.deleteRecording(id)
        if (status != EngineStatus.OK) "The recording could not be deleted: $status" else null
    }

    fun deleteNote(id: Long) {
        val engine = engine ?: return
        if (_uiState.value.playingNoteId == id) stopPlayback()
        viewModelScope.launch(Dispatchers.Default) {
            val status = engine.deleteNote(id)
            _uiState.update {
                it.copy(
                    notes = engine.listNotes(),
                    screen = if (it.screen == Screen.Note(id)) Screen.Notes else it.screen,
                    note = if (it.note?.id == id) null else it.note,
                    message = if (status != EngineStatus.OK) "Delete failed: $status" else null,
                )
            }
        }
    }

    /**
     * The transcript as text, with the summary above it when the note has one.
     * Without names, the summary is left out too: it names people.
     */
    fun share(id: Long, anonymise: Boolean) {
        val engine = engine ?: return
        viewModelScope.launch(Dispatchers.Default) {
            val note = engine.loadNote(id) ?: return@launch
            val transcript = engine.exportTranscript(id, includeTimestamps = true, anonymise = anonymise)
            if (transcript.isEmpty()) return@launch
            val text = if (anonymise || note.summary.isBlank()) transcript else withSummary(transcript, note.summary)
            _uiState.update { it.copy(share = ShareRequest.Text(note.title, text)) }
        }
    }

    /** The title and date lines, the summary (with its own headings), then the turns. */
    private fun withSummary(transcript: String, summary: String): String {
        val lines = transcript.lines()
        val head = lines.take(2).joinToString("\n")
        val body = lines.drop(2).joinToString("\n").trimStart('\n')
        return "$head\n\n${summary.trim()}\n\nTranscript:\n$body"
    }

    /** The transcript and summary as a PDF, for people who want a document. */
    fun sharePdf(id: Long) {
        exportFile(id, "Preparing the PDF") { engine, note, onProgress ->
            val transcript = engine.exportTranscript(id, includeTimestamps = true, anonymise = false)
            ShareFiles.target(getApplication(), note.title, "pdf").also { file ->
                ShareFiles.writePdf(file, transcript, note.summary, note.speakers.map { it.displayName })
                onProgress(1f)
            } to "application/pdf"
        }
    }

    /** The recording: as a small M4A (AAC), or the original WAV as recorded. */
    fun shareRecording(id: Long, compressed: Boolean) {
        exportFile(id, "Preparing the recording") { _, note, onProgress ->
            val wav = File(note.audioPath ?: return@exportFile null)
            if (!wav.exists()) return@exportFile null
            if (compressed) {
                ShareFiles.target(getApplication(), note.title, "m4a").also { file ->
                    ShareFiles.encodeM4a(wav, file, onProgress)
                } to "audio/mp4"
            } else {
                // Shared through the FileProvider, which only exposes share/.
                ShareFiles.target(getApplication(), note.title, "wav").also { file ->
                    wav.copyTo(file, overwrite = true)
                } to "audio/wav"
            }
        }
    }

    private fun exportFile(
        id: Long,
        step: String,
        make: suspend (TranscriptionEngine, NoteDetail, (Float) -> Unit) -> Pair<File, String>?,
    ) {
        val engine = engine ?: return
        _uiState.update { it.copy(busy = true, busyStep = "$step…") }
        viewModelScope.launch(Dispatchers.IO) {
            val result = runCatching {
                val note = engine.loadNote(id) ?: return@runCatching null
                var shown = -1
                make(engine, note) { progress ->
                    val percent = (progress * 100).toInt()
                    if (percent != shown) {
                        shown = percent
                        _uiState.update { it.copy(busyStep = "$step — $percent%") }
                    }
                }?.let { (file, mime) -> ShareRequest.Attachment(note.title, file, mime) }
            }
            _uiState.update {
                it.copy(
                    busy = false,
                    busyStep = null,
                    share = result.getOrNull() ?: it.share,
                    message = when {
                        result.isFailure -> "That could not be prepared: ${result.exceptionOrNull()?.message}"
                        result.getOrNull() == null -> "There is nothing to share."
                        else -> it.message
                    },
                )
            }
        }
    }

    fun shareHandled() = _uiState.update { it.copy(share = null) }

    fun messageShown() = _uiState.update { it.copy(message = null) }

    private fun noteOperation(id: Long, work: suspend (TranscriptionEngine) -> String?) {
        val engine = engine ?: return
        _uiState.update { it.copy(busy = true) }
        viewModelScope.launch(Dispatchers.Default) {
            val message = work(engine)
            _uiState.update { it.copy(busy = false, message = message, notes = engine.listNotes()) }
            reloadNote(id)
        }
    }

    // -- Add-ons (ext/Extensions.kt) --------------------------------------------------

    override fun engine(): TranscriptionEngine? = engine

    override fun modelFile(fileName: String): File? =
        installer.catalog.firstOrNull { it.fileName == fileName }?.let(installer::fileFor)

    override fun requestModel(fileName: String) {
        installer.catalog.firstOrNull { it.fileName == fileName }?.let { setModelWanted(it, true) }
    }

    override fun showMessage(text: String) = _uiState.update { it.copy(message = text) }

    override fun reloadNote(id: Long) {
        val engine = engine ?: return
        viewModelScope.launch(Dispatchers.Default) {
            val note = engine.loadNote(id)
            _uiState.update { if (it.screen == Screen.Note(id)) it.copy(note = note) else it }
        }
    }

    // -- Playback -----------------------------------------------------------------

    fun play(note: NoteDetail) {
        val path = note.audioPath ?: return
        stopPlayback()
        _uiState.update { it.copy(playingNoteId = note.id) }
        playback.play(File(path)) {
            // Arrives on the playback thread once the file runs out.
            _uiState.update { if (it.playingNoteId == note.id) it.copy(playingNoteId = null) else it }
        }
    }

    fun stopPlayback() {
        playback.stop()
        _uiState.update { it.copy(playingNoteId = null) }
    }

    private fun processing(step: String) {
        _uiState.update { it.copy(phase = Phase.PROCESSING, step = step) }
        TranscriptionService.saving(getApplication(), step)
    }

    private fun fail(message: String) {
        TranscriptionService.hide(getApplication())
        _uiState.update { it.copy(phase = Phase.FAILED, failure = message) }
    }

    override fun onCleared() {
        super.onCleared()
        TranscriptionService.hide(getApplication())
        capture?.stop()
        playback.stop()
        streamJob?.cancel()
        engine?.close()
    }
}
