package com.scribatic.app.engine

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.withContext
import java.io.Closeable
import java.util.concurrent.atomic.AtomicLong

/**
 * Kotlin façade over `libscribatic_engine.so`.
 *
 * ## Threading contract
 * Every method that reaches inference is a `suspend` function that hops to
 * [Dispatchers.Default] — a pool sized to the CPU count and, critically, *not*
 * the main dispatcher. Whisper's encoder holds a core for hundreds of
 * milliseconds; running it on `Dispatchers.Main` drops frames on a 120 Hz
 * panel long before it ever produces a visible ANR.
 *
 * [pushAudio] is the deliberate exception: it is a plain, non-suspending
 * function because it is invoked from the AAudio callback thread, which must
 * never suspend, allocate, or contend on a lock.
 *
 * ## Lifetime
 * The native engine is owned through [Closeable], not through GC finalization.
 * A GGUF mapping is far too large to leave to the collector's discretion.
 */
class TranscriptionEngine private constructor(
    handle: Long,
    private val dispatcher: CoroutineDispatcher = Dispatchers.Default,
) : Closeable {

    private val nativeHandle = AtomicLong(handle)

    companion object {
        init {
            // Single shared object: the C++ core is statically linked into it,
            // so there is nothing else to load and no ordering to get wrong.
            System.loadLibrary("scribatic_engine")
        }

        /**
         * Allocates the native engine. Cheap — no weights are touched until
         * [warmUp] runs. Returns null if either model file is missing.
         */
        fun create(config: EngineConfig): TranscriptionEngine? {
            val bootstrap = TranscriptionEngine(0L)
            val handle = bootstrap.nativeCreate(
                whisperModelPath = config.whisperModelPath,
                llamaModelPath = config.llamaModelPath,
                databasePath = config.databasePath,
                threadCount = config.threadCount,
                useMemoryMapping = config.useMemoryMapping,
            )
            return if (handle == 0L) null else TranscriptionEngine(handle)
        }
    }

    /** mmaps the GGUF weights and primes the KV cache. Background only. */
    suspend fun warmUp(): EngineStatus = withContext(dispatcher) {
        EngineStatus.from(nativeWarmUp(nativeHandle.get()))
    }

    /** Called from `onStop()` to shrink the resident set before the LMK looks. */
    suspend fun hibernate() = withContext(dispatcher) {
        nativeHibernate(nativeHandle.get())
    }

    fun state(): EngineState = EngineState.from(nativeState(nativeHandle.get()))

    /**
     * Realtime audio ingress. **Not** a suspend function by design — call it
     * directly from the AAudio/AudioRecord callback thread.
     */
    fun pushAudio(pcm: FloatArray, frameCount: Int): EngineStatus =
        EngineStatus.from(nativePushAudio(nativeHandle.get(), pcm, frameCount))

    /**
     * Cold stream of finalised segments. Each emission is one completed
     * whisper pass; the collector is expected to be a ViewModel mapping into
     * a `StateFlow` that Compose observes.
     */
    fun transcriptionStream(pollIntervalMs: Long = 400L): Flow<List<TranscriptSegment>> = flow {
        while (true) {
            currentCoroutineContext().ensureActive()
            nativeRunTranscriptionPass(nativeHandle.get())
            val segments = decodeSegments(nativeDrainSegments(nativeHandle.get()))
            if (segments.isNotEmpty()) emit(segments)
            kotlinx.coroutines.delay(pollIntervalMs)
        }
    }.flowOn(dispatcher)

    suspend fun summarize(transcript: String): String = withContext(dispatcher) {
        nativeSummarize(nativeHandle.get(), transcript)
    }

    suspend fun indexNote(noteId: Long, text: String): EngineStatus = withContext(dispatcher) {
        EngineStatus.from(nativeIndexNote(nativeHandle.get(), noteId, text))
    }

    /** Thread-safe; observed by the ggml abort callback between graph nodes. */
    fun requestCancel() = nativeRequestCancel(nativeHandle.get())

    override fun close() {
        val handle = nativeHandle.getAndSet(0L)
        if (handle != 0L) nativeDestroy(handle)
    }

    /** Flat [startMs, endMs, text, confidence] tuples -> typed segments. */
    private fun decodeSegments(flat: Array<String>): List<TranscriptSegment> =
        flat.toList().chunked(4).mapNotNull { tuple ->
            if (tuple.size < 4) return@mapNotNull null
            TranscriptSegment(
                startMs = tuple[0].toLongOrNull() ?: 0L,
                endMs = tuple[1].toLongOrNull() ?: 0L,
                text = tuple[2],
                confidence = tuple[3].toFloatOrNull() ?: 0f,
            )
        }

    // -- JNI declarations -----------------------------------------------------
    private external fun nativeCreate(
        whisperModelPath: String,
        llamaModelPath: String,
        databasePath: String,
        threadCount: Int,
        useMemoryMapping: Boolean,
    ): Long

    private external fun nativeDestroy(handle: Long)
    private external fun nativeWarmUp(handle: Long): Int
    private external fun nativeHibernate(handle: Long)
    private external fun nativeState(handle: Long): Int
    private external fun nativePushAudio(handle: Long, pcm: FloatArray, frameCount: Int): Int
    private external fun nativeRunTranscriptionPass(handle: Long): Int
    private external fun nativeDrainSegments(handle: Long): Array<String>
    private external fun nativeSummarize(handle: Long, transcript: String): String
    private external fun nativeIndexNote(handle: Long, noteId: Long, text: String): Int
    private external fun nativeRequestCancel(handle: Long)
}
