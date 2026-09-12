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
    OUT_OF_MEMORY(9);

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
)

/**
 * Paths must resolve inside `Context.filesDir`. Nothing in this app ever reads
 * or writes shared storage, and no manifest permission grants it the option.
 */
data class EngineConfig(
    val whisperModelPath: String,
    val llamaModelPath: String,
    val databasePath: String,
    val threadCount: Int = Runtime.getRuntime().availableProcessors().coerceAtMost(4),
    val useMemoryMapping: Boolean = true,
)
