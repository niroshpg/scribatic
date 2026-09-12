package com.scribatic.app.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.scribatic.app.engine.EngineState
import com.scribatic.app.engine.TranscriptSegment
import com.scribatic.app.engine.TranscriptionEngine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

data class TranscriptUiState(
    val statusLabel: String = "Idle",
    val segments: List<TranscriptSegment> = emptyList(),
    val summary: String? = null,
)

/**
 * Owns the engine for the lifetime of the screen and marshals every native
 * call onto [Dispatchers.Default]. The main dispatcher is touched only to
 * publish an already-computed immutable [TranscriptUiState].
 */
class TranscriptionViewModel(
    private val engine: TranscriptionEngine? = null,
) : ViewModel() {

    private val _uiState = MutableStateFlow(TranscriptUiState())
    val uiState: StateFlow<TranscriptUiState> = _uiState.asStateFlow()

    fun start() {
        val engine = engine ?: return

        // Warm-up and the transcription loop both run off the main thread.
        viewModelScope.launch(Dispatchers.Default) {
            engine.warmUp()
            engine.transcriptionStream().collect { batch ->
                _uiState.value = _uiState.value.copy(
                    statusLabel = engine.state().name,
                    segments = _uiState.value.segments + batch,
                )
            }
        }
    }

    fun summarize() {
        val engine = engine ?: return
        viewModelScope.launch(Dispatchers.Default) {
            val transcript = _uiState.value.segments.joinToString(" ") { it.text }
            val summary = engine.summarize(transcript)
            _uiState.value = _uiState.value.copy(summary = summary)
        }
    }

    override fun onCleared() {
        super.onCleared()
        engine?.close()
    }
}
