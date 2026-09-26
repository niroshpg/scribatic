package com.scribatic.app.engine

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
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
 * Every other native call runs on ONE lane of that pool, never two at once —
 * the same serialisation the Swift actor gives iOS. The engine is not
 * re-entrant: a transcription pass still running on one pool thread while
 * Stop's flush starts on another is two decodes on one whisper context, and
 * that segfaults inside ggml. Suspension points (the polling delay) release
 * the lane, so the stream does not starve other calls.
 *
 * ## Lifetime
 * The native engine is owned through [Closeable], not through GC finalization.
 * A GGUF mapping is far too large to leave to the collector's discretion.
 */
class TranscriptionEngine private constructor(
    handle: Long,
    @OptIn(ExperimentalCoroutinesApi::class)
    private val dispatcher: CoroutineDispatcher = Dispatchers.Default.limitedParallelism(1),
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
                embedModelPath = config.embedModelPath,
                databasePath = config.databasePath,
                recordingsDirectory = config.recordingsDirectory,
                segmentationModelPath = config.segmentationModelPath,
                speakerEmbeddingModelPath = config.speakerEmbeddingModelPath,
                threadCount = config.threadCount,
                useMemoryMapping = config.useMemoryMapping,
            )
            return if (handle == 0L) null else TranscriptionEngine(handle)
        }

        /** Every model the app can use, from the core's single catalog. */
        fun modelCatalog(): List<ModelSpec> =
            nativeModelCatalog().toList().chunked(7).map { f ->
                ModelSpec(f[0], f[1], f[2], f[3], f[4].toLong(), f[5], f[6] == "1")
            }

        /** Opened in the system browser; the app never downloads anything. */
        fun modelDownloadPage(): String = nativeModelDownloadPage()

        @JvmStatic private external fun nativeModelCatalog(): Array<String>
        @JvmStatic private external fun nativeModelDownloadPage(): String
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

    /// Decodes whatever audio is still buffered, however short. Called when
    /// recording stops: the decode window is several seconds, so without this
    /// everything said since the last window boundary is discarded and the app
    /// appears to lose the end of the recording.
    suspend fun flush(): List<TranscriptSegment> = withContext(dispatcher) {
        nativeFlush(nativeHandle.get())
        decodeSegments(nativeDrainSegments(nativeHandle.get()))
    }

    suspend fun summarize(transcript: String): String = withContext(dispatcher) {
        nativeSummarize(nativeHandle.get(), transcript)
    }

    // -- Sessions --------------------------------------------------------------

    /** Before capture starts: resets the ring buffer and the timeline. */
    suspend fun beginSession() = withContext(dispatcher) {
        nativeBeginSession(nativeHandle.get())
    }

    /** Persists the finished session; call after [flush]. Returns 0 on failure. */
    suspend fun saveSession(title: String, createdAtSeconds: Long, audioPath: String?): Long =
        withContext(dispatcher) {
            nativeSaveSession(nativeHandle.get(), title, createdAtSeconds, audioPath.orEmpty())
        }

    // -- Notes -------------------------------------------------------------------

    suspend fun listNotes(): List<NoteSummary> = withContext(dispatcher) {
        nativeListNotes(nativeHandle.get()).toList().chunked(NOTE_FIELDS).mapNotNull { f ->
            if (f.size < NOTE_FIELDS) return@mapNotNull null
            NoteSummary(
                id = f[0].toLong(),
                title = f[1],
                createdAtSeconds = f[2].toLong(),
                durationMs = f[3].toLong(),
                hasAudio = f[4] == "1",
                speakerCount = f[5].toInt(),
                preview = f[6],
            )
        }
    }

    suspend fun loadNote(noteId: Long): NoteDetail? = withContext(dispatcher) {
        decodeNote(nativeLoadNote(nativeHandle.get(), noteId))
    }

    suspend fun deleteRecording(noteId: Long): EngineStatus = withContext(dispatcher) {
        EngineStatus.from(nativeDeleteRecording(nativeHandle.get(), noteId))
    }

    suspend fun deleteNote(noteId: Long): EngineStatus = withContext(dispatcher) {
        EngineStatus.from(nativeDeleteNote(nativeHandle.get(), noteId))
    }

    suspend fun exportTranscript(noteId: Long, includeTimestamps: Boolean, anonymise: Boolean): String =
        withContext(dispatcher) {
            nativeExportTranscript(nativeHandle.get(), noteId, includeTimestamps, anonymise)
        }

    // -- Speakers ----------------------------------------------------------------

    /** Stats two model files; cheap and safe from any thread. */
    fun canIdentifySpeakers(): Boolean = nativeCanIdentifySpeakers(nativeHandle.get())

    /** Diarizes the note's recording. [expected] 0 estimates the count. */
    suspend fun identifySpeakers(noteId: Long, expected: Int = 0): EngineStatus = withContext(dispatcher) {
        EngineStatus.from(nativeIdentifySpeakers(nativeHandle.get(), noteId, expected))
    }

    suspend fun renameSpeaker(noteId: Long, speaker: Int, name: String): EngineStatus =
        withContext(dispatcher) {
            EngineStatus.from(nativeRenameSpeaker(nativeHandle.get(), noteId, speaker, name))
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

    /** Flat [startMs, endMs, text, confidence, speaker] tuples -> typed segments. */
    private fun decodeSegments(flat: Array<String>): List<TranscriptSegment> =
        flat.toList().chunked(SEGMENT_FIELDS).mapNotNull { tuple ->
            if (tuple.size < SEGMENT_FIELDS) return@mapNotNull null
            segmentOf(tuple, 0)
        }

    private fun segmentOf(f: List<String>, at: Int) = TranscriptSegment(
        startMs = f[at].toLongOrNull() ?: 0L,
        endMs = f[at + 1].toLongOrNull() ?: 0L,
        text = f[at + 2],
        confidence = f[at + 3].toFloatOrNull() ?: 0f,
        speaker = f[at + 4].toIntOrNull() ?: -1,
    )

    /**
     * Layout written by nativeLoadNote: six header fields, then a count and
     * that many (index, name, displayName) triples, then a count and that
     * many segment tuples. Empty array means no such note.
     */
    private fun decodeNote(flat: Array<String>): NoteDetail? {
        if (flat.size < 8) return null
        val f = flat.toList()
        var at = 6
        val speakerCount = f[at++].toInt()
        val speakers = (0 until speakerCount).map {
            SpeakerLabel(f[at].toInt(), f[at + 1], f[at + 2]).also { at += 3 }
        }
        val segmentCount = f[at++].toInt()
        val segments = (0 until segmentCount).map {
            segmentOf(f, at).also { at += SEGMENT_FIELDS }
        }
        return NoteDetail(
            id = f[0].toLong(),
            title = f[1],
            createdAtSeconds = f[2].toLong(),
            durationMs = f[3].toLong(),
            audioPath = f[4].ifEmpty { null },
            speakerCount = f[5].toInt(),
            speakers = speakers,
            segments = segments,
        )
    }

    // -- JNI declarations -----------------------------------------------------
    private external fun nativeCreate(
        whisperModelPath: String,
        llamaModelPath: String,
        embedModelPath: String,
        databasePath: String,
        recordingsDirectory: String,
        segmentationModelPath: String,
        speakerEmbeddingModelPath: String,
        threadCount: Int,
        useMemoryMapping: Boolean,
    ): Long

    private external fun nativeDestroy(handle: Long)
    private external fun nativeWarmUp(handle: Long): Int
    private external fun nativeHibernate(handle: Long)
    private external fun nativeState(handle: Long): Int
    private external fun nativePushAudio(handle: Long, pcm: FloatArray, frameCount: Int): Int
    private external fun nativeRunTranscriptionPass(handle: Long): Int
    private external fun nativeFlush(handle: Long): Int
    private external fun nativeDrainSegments(handle: Long): Array<String>
    private external fun nativeSummarize(handle: Long, transcript: String): String
    private external fun nativeIndexNote(handle: Long, noteId: Long, text: String): Int
    private external fun nativeRequestCancel(handle: Long)
    private external fun nativeBeginSession(handle: Long)
    private external fun nativeSaveSession(handle: Long, title: String, createdAt: Long, audioPath: String): Long
    private external fun nativeListNotes(handle: Long): Array<String>
    private external fun nativeLoadNote(handle: Long, noteId: Long): Array<String>
    private external fun nativeDeleteRecording(handle: Long, noteId: Long): Int
    private external fun nativeDeleteNote(handle: Long, noteId: Long): Int
    private external fun nativeExportTranscript(
        handle: Long,
        noteId: Long,
        includeTimestamps: Boolean,
        anonymise: Boolean,
    ): String
    private external fun nativeCanIdentifySpeakers(handle: Long): Boolean
    private external fun nativeIdentifySpeakers(handle: Long, noteId: Long, expected: Int): Int
    private external fun nativeRenameSpeaker(handle: Long, noteId: Long, speaker: Int, name: String): Int
}

private const val SEGMENT_FIELDS = 5
private const val NOTE_FIELDS = 7
