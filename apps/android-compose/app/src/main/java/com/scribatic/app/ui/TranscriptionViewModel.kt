package com.scribatic.app.ui

import android.app.Application
import android.text.format.DateFormat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.scribatic.app.BuildConfig
import com.scribatic.app.audio.AudioCapture
import com.scribatic.app.audio.AudioPlayback
import com.scribatic.app.engine.EngineConfig
import com.scribatic.app.engine.EngineStatus
import com.scribatic.app.engine.NoteDetail
import com.scribatic.app.engine.NoteSummary
import com.scribatic.app.engine.TranscriptSegment
import com.scribatic.app.engine.TranscriptionEngine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.io.File
import java.util.Date

/** What the recorder is doing, as the UI needs to understand it. */
enum class Phase { STARTING, READY, RECORDING, PAUSED, PROCESSING, FAILED }

/** Which screen is showing. The notes list is the root; the others sit over it. */
sealed interface Screen {
    data object Notes : Screen
    data object Recorder : Screen
    data class Note(val id: Long) : Screen
}

/** Text the activity should hand to the system share sheet, once. */
data class ShareRequest(val subject: String, val text: String)

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
    val message: String? = null,
    val share: ShareRequest? = null,
) {
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
class TranscriptionViewModel(application: Application) : AndroidViewModel(application) {

    private val _uiState = MutableStateFlow(TranscriptUiState())
    val uiState: StateFlow<TranscriptUiState> = _uiState.asStateFlow()

    private var engine: TranscriptionEngine? = null
    private var streamJob: Job? = null
    private var capture: AudioCapture? = null
    private var recordingFile: File? = null
    private var recordingStartedAt = 0L
    private val playback = AudioPlayback()

    private val filesDir: File get() = getApplication<Application>().filesDir

    /** Inside filesDir, so covered by allowBackup=false and the extraction rules. */
    private val recordingsDir: File get() = File(filesDir, "recordings").apply { mkdirs() }

    /**
     * Warms the engine only. The microphone is deliberately NOT opened here: for
     * an app whose whole claim is that audio never leaves the device, a mic that
     * goes live the instant the app launches is the wrong default.
     */
    fun prepare() {
        if (engine != null) return

        viewModelScope.launch(Dispatchers.Default) {
            val config = EngineConfig(
                whisperModelPath = File(filesDir, "ggml-base.en.bin").absolutePath,
                llamaModelPath = File(filesDir, "insight-q4_k_m.gguf").absolutePath,
                embedModelPath = File(filesDir, "embed-minilm-l6-v2.gguf").absolutePath,
                databasePath = File(File(filesDir, "store").apply { mkdirs() }, "scribatic.sqlite").absolutePath,
                recordingsDirectory = recordingsDir.absolutePath,
                segmentationModelPath = File(filesDir, "speaker-segmentation.onnx").absolutePath,
                speakerEmbeddingModelPath = File(filesDir, "speaker-embedding.onnx").absolutePath,
            )

            val created = TranscriptionEngine.create(config)
            if (created == null) {
                fail("Engine could not be created — are the model files in ${filesDir.absolutePath}?")
                return@launch
            }

            val status = created.warmUp()
            if (status != EngineStatus.OK) {
                fail("Model load failed: $status")
                created.close()
                return@launch
            }

            engine = created
            _uiState.update {
                it.copy(
                    phase = Phase.READY,
                    failure = null,
                    canIdentifySpeakers = created.canIdentifySpeakers(),
                    notes = created.listNotes(),
                )
            }
        }
    }

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

        viewModelScope.launch(Dispatchers.Default) {
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

            streamJob?.cancel()
            streamJob = viewModelScope.launch(Dispatchers.Default) {
                engine.transcriptionStream().collect { batch ->
                    _uiState.update { it.copy(segments = it.segments + batch) }
                }
            }
        }
    }

    fun pauseRecording() {
        if (_uiState.value.phase != Phase.RECORDING) return
        capture?.pause()
        _uiState.update { it.copy(phase = Phase.PAUSED) }
    }

    fun resumeRecording() {
        if (_uiState.value.phase != Phase.PAUSED) return
        capture?.resume()
        _uiState.update { it.copy(phase = Phase.RECORDING) }
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

            var message: String? = null
            if (_uiState.value.canIdentifySpeakers) {
                processing("Identifying speakers")
                val status = engine.identifySpeakers(id)
                if (status != EngineStatus.OK) message = "Speakers could not be identified: $status"
            }

            _uiState.update {
                it.copy(
                    phase = Phase.READY,
                    segments = emptyList(),
                    notes = engine.listNotes(),
                    screen = Screen.Note(id),
                    message = message,
                )
            }
            reloadNote(id)
        }
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

    fun share(id: Long, anonymise: Boolean) {
        val engine = engine ?: return
        viewModelScope.launch(Dispatchers.Default) {
            val text = engine.exportTranscript(id, includeTimestamps = true, anonymise = anonymise)
            val subject = _uiState.value.note?.title.orEmpty()
            if (text.isNotEmpty()) _uiState.update { it.copy(share = ShareRequest(subject, text)) }
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

    private fun reloadNote(id: Long) {
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

    private fun processing(step: String) = _uiState.update { it.copy(phase = Phase.PROCESSING, step = step) }

    private fun fail(message: String) {
        _uiState.update { it.copy(phase = Phase.FAILED, failure = message) }
    }

    override fun onCleared() {
        super.onCleared()
        capture?.stop()
        playback.stop()
        streamJob?.cancel()
        engine?.close()
    }
}
