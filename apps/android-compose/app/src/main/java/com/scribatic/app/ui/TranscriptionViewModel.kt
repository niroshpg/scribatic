package com.scribatic.app.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.scribatic.app.audio.AudioCapture
import com.scribatic.app.audio.AudioPlayback
import com.scribatic.app.engine.EngineConfig
import com.scribatic.app.engine.EngineStatus
import com.scribatic.app.engine.TranscriptSegment
import com.scribatic.app.engine.TranscriptionEngine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.File

/** What the screen is doing, as the UI needs to understand it. */
enum class Phase { STARTING, READY, RECORDING, PAUSED, PLAYING, FAILED }

data class TranscriptUiState(
    val phase: Phase = Phase.STARTING,
    val segments: List<TranscriptSegment> = emptyList(),
    val summary: String? = null,
    val failure: String? = null,
    val hasRecording: Boolean = false,
) {
    val canClear: Boolean get() = segments.isNotEmpty() || hasRecording

    val label: String
        get() = when (phase) {
            Phase.STARTING  -> "Preparing"
            Phase.READY     -> "Ready"
            Phase.RECORDING -> "Recording"
            Phase.PAUSED    -> "Paused"
            Phase.PLAYING   -> "Playing"
            Phase.FAILED    -> "Stopped"
        }
}

/**
 * Owns the engine for the lifetime of the screen and marshals every native call
 * onto [Dispatchers.Default]. The main dispatcher is touched only to publish an
 * already-computed immutable [TranscriptUiState].
 *
 * This is an [AndroidViewModel] because the engine's paths are resolved against
 * `filesDir`, which needs a Context. Previously the engine was a constructor
 * parameter defaulting to null and nothing ever supplied one, so the whole
 * native side was unreachable from the app.
 */
class TranscriptionViewModel(application: Application) : AndroidViewModel(application) {

    private val _uiState = MutableStateFlow(TranscriptUiState())
    val uiState: StateFlow<TranscriptUiState> = _uiState.asStateFlow()

    private var engine: TranscriptionEngine? = null
    private var streamJob: Job? = null
    private var capture: AudioCapture? = null
    private val playback = AudioPlayback()

    private val filesDir: File get() = getApplication<Application>().filesDir
    private val recordingFile: File get() = File(filesDir, "recording.pcmf32")

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
                databasePath = File(filesDir, "scribatic.sqlite").absolutePath,
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
            _uiState.value = _uiState.value.copy(phase = Phase.READY, failure = null)
        }
    }

    fun startRecording() {
        val engine = engine ?: return
        if (_uiState.value.phase == Phase.RECORDING) return

        val audio = AudioCapture(recordingFile)
        try {
            audio.start { buffer, frameCount ->
                // Capture thread. Lock-free on the C++ side, which is what makes
                // calling straight through from here safe.
                engine.pushAudio(buffer, frameCount)
            }
        } catch (t: Throwable) {
            fail(t.message ?: "Microphone could not be opened")
            return
        }

        capture = audio
        _uiState.value = _uiState.value.copy(phase = Phase.RECORDING, failure = null)

        streamJob?.cancel()
        streamJob = viewModelScope.launch(Dispatchers.Default) {
            engine.transcriptionStream().collect { batch ->
                _uiState.value = _uiState.value.copy(segments = _uiState.value.segments + batch)
            }
        }
    }

    fun pauseRecording() {
        if (_uiState.value.phase != Phase.RECORDING) return
        capture?.pause()
        _uiState.value = _uiState.value.copy(phase = Phase.PAUSED)
    }

    fun resumeRecording() {
        if (_uiState.value.phase != Phase.PAUSED) return
        capture?.resume()
        _uiState.value = _uiState.value.copy(phase = Phase.RECORDING)
    }

    fun stopRecording() {
        val phase = _uiState.value.phase
        if (phase != Phase.RECORDING && phase != Phase.PAUSED) return

        capture?.stop()
        capture = null
        streamJob?.cancel()
        streamJob = null

        _uiState.value = _uiState.value.copy(
            phase = Phase.READY,
            hasRecording = recordingFile.length() > 0,
        )

        // Decode the tail. The window is several seconds wide, so without this
        // everything said since the last boundary is never transcribed.
        viewModelScope.launch(Dispatchers.Default) {
            val tail = engine?.flush().orEmpty()
            if (tail.isNotEmpty()) {
                _uiState.value = _uiState.value.copy(segments = _uiState.value.segments + tail)
            }
        }
    }

    /** Plays the capture back. Raw float32 in, same format straight out. */
    fun play() {
        if (!_uiState.value.hasRecording) return
        if (_uiState.value.phase != Phase.READY) return

        _uiState.value = _uiState.value.copy(phase = Phase.PLAYING)
        playback.play(recordingFile) {
            // Arrives on the playback thread once the file runs out.
            _uiState.value = _uiState.value.copy(
                phase = if (_uiState.value.phase == Phase.PLAYING) Phase.READY else _uiState.value.phase,
            )
        }
    }

    fun stopPlayback() {
        playback.stop()
        if (_uiState.value.phase == Phase.PLAYING) {
            _uiState.value = _uiState.value.copy(phase = Phase.READY)
        }
    }

    /** Discards the transcript and the captured audio together. */
    fun clear() {
        stopPlayback()
        recordingFile.delete()
        _uiState.value = _uiState.value.copy(
            segments = emptyList(),
            summary = null,
            hasRecording = false,
            phase = if (_uiState.value.phase == Phase.FAILED) Phase.FAILED else Phase.READY,
        )
    }

    fun summarize() {
        val engine = engine ?: return
        viewModelScope.launch(Dispatchers.Default) {
            val transcript = _uiState.value.segments.joinToString(" ") { it.text }
            _uiState.value = _uiState.value.copy(summary = engine.summarize(transcript))
        }
    }

    private fun fail(message: String) {
        _uiState.value = _uiState.value.copy(phase = Phase.FAILED, failure = message)
    }

    override fun onCleared() {
        super.onCleared()
        capture?.stop()
        playback.stop()
        streamJob?.cancel()
        engine?.close()
    }
}
