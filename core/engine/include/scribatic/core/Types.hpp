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
    std::string  databasePath;        ///< SQLite file hosting the VSS index
    std::int32_t threadCount    = 4;  ///< pinned to performance cores only
    std::int32_t contextWindow  = 4096;
    bool         useMemoryMapping = true;  ///< mmap GGUF instead of read()
    bool         useGpuDelegate   = false; ///< Metal (iOS) / Vulkan (Android)
    bool         enableVad        = true;
};

} // namespace scribatic::core
