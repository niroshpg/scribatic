// =============================================================================
//  Types.hpp — Value types crossing the C++ <-> Swift / C++ <-> JNI boundary.
//
//  DESIGN CONSTRAINT
//  -----------------
//  Every type declared here must be *interop safe*. Under Swift-C++ Interop a
//  header is imported wholesale; any construct the importer cannot model poisons
//  the entire module. We therefore restrict the public surface to:
//      - trivially copyable scalars and enums with fixed underlying types
//      - std::string / std::vector (natively bridged by the Swift importer)
//      - concrete classes with value semantics, or reference types explicitly
//        annotated via <swift/bridging>
//  Explicitly banned from this header: std::function, raw owning pointers,
//  variadic templates, and anything requiring an Objective-C++ (.mm) shim.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scribatic::core {

/// Canonical result code. Mirrored 1:1 by Swift `EngineStatus` and Kotlin
/// `EngineStatus` so no error-mapping table has to be maintained per platform.
enum class EngineStatus : std::int32_t {
    Ok                  = 0,
    NotInitialized      = 1,
    ModelNotFound       = 2,
    ModelLoadFailed     = 3,
    UnsupportedFormat   = 4,
    AudioBufferOverrun  = 5,
    InferenceFailed     = 6,
    DatabaseFailed      = 7,
    Cancelled           = 8,
    OutOfMemory         = 9,
    RecordingNotFound   = 10, ///< the note has no audio, or the file is gone
    SpeakersUnavailable = 11, ///< diarization models or library not present
    FileRemovalFailed   = 12, ///< the OS refused to delete a recording
};

/// Lifecycle of the engine's inference pipeline, polled by the UI layer.
enum class EngineState : std::int32_t {
    Idle        = 0,
    Warming     = 1,  ///< mmap'ing weights, priming KV cache
    Listening   = 2,  ///< ring buffer accepting PCM, below VAD threshold
    Transcribing= 3,  ///< whisper.cpp encoder/decoder active
    Summarizing = 4,  ///< llama.cpp decode loop active
    Faulted     = 5,
};

/// Immutable unit of recognised speech emitted by the whisper backend.
struct TranscriptSegment {
    std::int64_t startMs   = 0;
    std::int64_t endMs     = 0;
    std::string  text;
    float        confidence = 0.0F;   ///< mean token logprob, normalised 0..1
    bool         isFinal    = false;  ///< false => provisional streaming hypothesis
    /// Zero-based speaker within its note, or -1 before speakers have been
    /// identified (and for any stretch diarization could not attribute).
    std::int32_t speaker    = -1;
};

/// One voice in a note. Speakers are per note: "Speaker 1" in one recording
/// has no relationship to "Speaker 1" in another. Nothing here is a voiceprint
/// — see ADR-009 for why none is ever stored.
struct SpeakerLabel {
    std::int32_t index = 0;
    std::string  name;          ///< what the user called them; empty if unnamed
    std::string  displayName;   ///< `name`, or "Speaker N" — never empty
};

/// A row in the notes list.
struct NoteSummary {
    std::int64_t id           = 0;
    std::string  title;
    std::int64_t createdAt    = 0;     ///< unix epoch, seconds
    std::int64_t durationMs   = 0;
    bool         hasAudio     = false; ///< false once the recording is deleted
    std::int32_t speakerCount = 0;     ///< 0 until speakers are identified
    std::string  preview;              ///< opening words of the transcript
};

/// Everything the note screen shows. `id == 0` means no such note.
struct NoteDetail {
    std::int64_t                   id           = 0;
    std::string                    title;
    std::int64_t                   createdAt    = 0;
    std::int64_t                   durationMs   = 0;
    std::string                    audioPath;   ///< absolute; empty once deleted
    std::int32_t                   speakerCount = 0;
    std::vector<SpeakerLabel>      speakers;
    std::vector<TranscriptSegment> segments;
};

/// A row returned from the SQLite-VSS approximate nearest-neighbour index.
struct RetrievalHit {
    std::int64_t noteId   = 0;
    std::int64_t chunkId  = 0;
    std::string  snippet;
    float        distance = 0.0F;     ///< L2 distance; lower is closer
};

/// Engine construction parameters. Paths are absolute and platform-supplied:
///   iOS     -> FileManager container URL (NSFileProtectionComplete)
///   Android -> Context.getFilesDir() (app-private, no MANAGE_EXTERNAL_STORAGE)
struct EngineConfig {
    std::string  whisperModelPath;    ///< ggml/GGUF acoustic model
    std::string  llamaModelPath;      ///< GGUF instruct model for local insight
    /// GGUF embedding model. Separate from the instruct model on purpose: an
    /// instruct model is a poor embedder and does not emit the 384 dimensions
    /// the vss0 table is declared with. See ADR-007.
    std::string  embedModelPath;
    std::string  databasePath;        ///< SQLite file hosting the VSS index
    /// Directory the platform writes recordings into. Notes store a file name
    /// relative to it, never an absolute path: an iOS app container moves on
    /// every reinstall, and an absolute path would dangle after the first one.
    std::string  recordingsDirectory;
    /// Speaker diarization (ADR-009). Optional: if either file is missing the
    /// engine still transcribes, and `canIdentifySpeakers()` reports false.
    std::string  segmentationModelPath;
    std::string  speakerEmbeddingModelPath;
    std::int32_t threadCount    = 4;  ///< pinned to performance cores only
    std::int32_t contextWindow  = 4096;
    bool         useMemoryMapping = true;  ///< mmap GGUF instead of read()
    bool         useGpuDelegate   = false; ///< Metal (iOS) / Vulkan (Android)
    bool         enableVad        = true;
};

} // namespace scribatic::core
