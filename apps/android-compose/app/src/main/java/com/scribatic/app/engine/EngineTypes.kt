package com.scribatic.app.engine

/**
 * Kotlin mirrors of `scribatic::core` value types. The ordinals are pinned to the
 * C++ enum values in `Types.hpp`; changing one without the other is a bug that
 * the unit test in `EngineStatusTest` is there to catch.
 */
enum class EngineStatus(val code: Int) {
    OK(0),
    NOT_INITIALIZED(1),
    MODEL_NOT_FOUND(2),
    MODEL_LOAD_FAILED(3),
    UNSUPPORTED_FORMAT(4),
    AUDIO_BUFFER_OVERRUN(5),
    INFERENCE_FAILED(6),
    DATABASE_FAILED(7),
    CANCELLED(8),
    OUT_OF_MEMORY(9),
    RECORDING_NOT_FOUND(10),
    SPEAKERS_UNAVAILABLE(11),
    FILE_REMOVAL_FAILED(12);

    companion object {
        fun from(code: Int): EngineStatus =
            entries.firstOrNull { it.code == code } ?: INFERENCE_FAILED
    }
}

enum class EngineState(val code: Int) {
    IDLE(0),
    WARMING(1),
    LISTENING(2),
    TRANSCRIBING(3),
    SUMMARIZING(4),
    FAULTED(5);

    companion object {
        fun from(code: Int): EngineState =
            entries.firstOrNull { it.code == code } ?: FAULTED
    }
}

data class TranscriptSegment(
    val startMs: Long,
    val endMs: Long,
    val text: String,
    val confidence: Float,
    /** Zero-based speaker within the note; -1 until speakers are identified. */
    val speaker: Int = -1,
)

/** One voice in one note. `displayName` is the name, or "Speaker N". */
data class SpeakerLabel(
    val index: Int,
    val name: String,
    val displayName: String,
)

data class NoteSummary(
    val id: Long,
    val title: String,
    val createdAtSeconds: Long,
    val durationMs: Long,
    val hasAudio: Boolean,
    val speakerCount: Int,
    val preview: String,
)

data class NoteDetail(
    val id: Long,
    val title: String,
    val createdAtSeconds: Long,
    val durationMs: Long,
    /** Absolute path of the recording; null once it has been deleted. */
    val audioPath: String?,
    val speakerCount: Int,
    val speakers: List<SpeakerLabel>,
    val segments: List<TranscriptSegment>,
) {
    fun speakerName(index: Int): String? = speakers.firstOrNull { it.index == index }?.displayName
}

/**
 * Paths must resolve inside `Context.filesDir`. Nothing in this app ever reads
 * or writes shared storage, and no manifest permission grants it the option.
 */
data class EngineConfig(
    val whisperModelPath: String,
    val llamaModelPath: String,
    val embedModelPath: String,
    val databasePath: String,
    /** Recordings live here; notes store a file name relative to it. */
    val recordingsDirectory: String,
    val segmentationModelPath: String,
    val speakerEmbeddingModelPath: String,
    val threadCount: Int = Runtime.getRuntime().availableProcessors().coerceAtMost(4),
    val useMemoryMapping: Boolean = true,
)

/** One model file, from the shared catalog in the C++ core. Identified by hash. */
data class ModelSpec(
    val fileName: String,
    val title: String,
    val purpose: String,
    /** For an optional model: what stops working without it. */
    val withoutIt: String,
    val sizeBytes: Long,
    val sha256: String,
    val required: Boolean,
)
