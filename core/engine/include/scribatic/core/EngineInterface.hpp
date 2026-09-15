// =============================================================================
//  EngineInterface.hpp — The single public entry point of the shared core.
//
//  This is the *only* header both platforms are permitted to import. iOS reaches
//  it through `apps/ios-swiftui/ScribaticCore/module.modulemap` (direct Swift-C++
//  Interop, no .mm wrapper); Android reaches it through the thin JNI translation
//  unit at `apps/android-compose/app/src/main/cpp/native-lib.cpp`.
//
//  CONCURRENCY CONTRACT
//  --------------------
//  `pushAudio()` is lock-free and realtime-safe: it is the ONLY method callable
//  from an audio callback thread (AVAudioEngine tap / AAudio callback). It never
//  allocates, never locks, and never blocks.
//  Every other method is blocking and MUST be invoked from a background
//  executor — a Swift `Task.detached` / custom actor executor, or a Kotlin
//  `Dispatchers.Default` coroutine. Calling `runTranscriptionPass()` on a main
//  thread will stall the render loop for hundreds of milliseconds and is a
//  programming error, not a performance hint.
//
//  OWNERSHIP CONTRACT
//  ------------------
//  Instances are heap-allocated and reference counted by the host language.
//  Swift adopts them as a SWIFT_SHARED_REFERENCE class so ARC drives the
//  lifetime; Kotlin holds an opaque `long` handle released in `close()`.
// =============================================================================
#pragma once

#include "scribatic/core/Types.hpp"

#include <memory>

#if __has_include(<swift/bridging>)
#  include <swift/bridging>
#else
#  define SWIFT_SHARED_REFERENCE(retain, release)
#  define SWIFT_RETURNS_INDEPENDENT_VALUE
#  define SWIFT_NONCOPYABLE
#endif

namespace scribatic::core {
class EngineInterface;
} // namespace scribatic::core

// Retain/release shims for the Swift importer. These sit at GLOBAL scope on
// purpose: SWIFT_SHARED_REFERENCE resolves the two names it is given in the
// global namespace, and a `scribatic::core::` qualified pair fails to resolve
// with "cannot find retain function". They are declared before the class so
// the attribute can name them.
void scribaticEngineRetain(scribatic::core::EngineInterface* engine) noexcept;
void scribaticEngineRelease(scribatic::core::EngineInterface* engine) noexcept;

namespace scribatic::core {

/// Abstract facade over the whisper.cpp + llama.cpp + SQLite-VSS pipeline.
/// Concrete implementation lives in `src/EngineImpl.cpp`; neither app links
/// against, or can even see, the backend headers.
class SWIFT_SHARED_REFERENCE(scribaticEngineRetain, scribaticEngineRelease) EngineInterface {
public:
    /// Factory. Returns nullptr on failure and writes the reason to `outStatus`.
    /// Does NOT touch the filesystem beyond an existence check — actual weight
    /// loading is deferred to `warmUp()` so construction stays cheap.
    static EngineInterface* create(const EngineConfig& config,
                                   EngineStatus* outStatus) noexcept;

    virtual ~EngineInterface() = default;

    // -- Lifecycle ----------------------------------------------------------
    /// mmap()s both GGUF models, allocates the ggml scratch arenas and primes
    /// the KV cache. Blocking; expect 200–900 ms on an A17/SD8Gen3 class device.
    virtual EngineStatus warmUp() noexcept = 0;

    /// Releases ggml contexts and munmap()s weights while keeping the object
    /// alive. Invoked on `scenePhase == .background` / `onStop()` so the OS
    /// low-memory killer sees a small resident set.
    virtual void hibernate() noexcept = 0;

    [[nodiscard]] virtual EngineState state() const noexcept = 0;

    // -- Realtime audio path (lock-free, callback-thread safe) --------------
    /// Copies `frameCount` mono float samples (16 kHz, [-1, 1]) into the SPSC
    /// ring buffer. Returns AudioBufferOverrun if the consumer has fallen
    /// behind; the caller must drop, never block.
    virtual EngineStatus pushAudio(const float* pcmFrames,
                                   std::size_t frameCount) noexcept = 0;

    // -- Inference (background threads only) -------------------------------
    /// Drains the ring buffer and runs one whisper.cpp encode/decode pass.
    ///
    /// Audio is accumulated into a decode window rather than transcribed the
    /// instant it arrives: the caller polls several times a second, and a
    /// fraction of a second of speech gives the decoder almost nothing to work
    /// with. A pass therefore often buffers and returns without producing a
    /// segment, which is not an error.
    virtual EngineStatus runTranscriptionPass() noexcept = 0;

    /// Decodes whatever audio is still buffered, however short.
    ///
    /// Call this when recording stops. Without it the tail of every recording —
    /// anything accumulated since the last full window — is silently discarded,
    /// which reads as the app dropping the end of the last sentence.
    virtual EngineStatus flush() noexcept = 0;

    /// Moves finalised segments out of the engine. Returning by value keeps the
    /// Swift importer on the happy path (no callbacks, no escaping pointers).
    [[nodiscard]] virtual std::vector<TranscriptSegment> drainSegments() = 0;

    /// Runs the llama.cpp decode loop over `transcript` using the local insight
    /// prompt template. Blocking, cancellable via `requestCancel()`.
    [[nodiscard]] virtual std::string summarize(const std::string& transcript) = 0;

    // -- Retrieval-augmented history ---------------------------------------
    /// Embeds `text` and upserts it into the SQLite-VSS index.
    virtual EngineStatus indexNote(std::int64_t noteId, const std::string& text) = 0;

    /// Approximate k-NN search across every note ever transcribed on-device.
    [[nodiscard]] virtual std::vector<RetrievalHit> search(const std::string& query,
                                                           std::int32_t topK) = 0;

    /// Cooperative cancellation. Safe to call from any thread; the ggml abort
    /// callback observes the flag between graph nodes.
    virtual void requestCancel() noexcept = 0;

protected:
    EngineInterface() = default;
    EngineInterface(const EngineInterface&) = delete;
    EngineInterface& operator=(const EngineInterface&) = delete;
};

/// Human-readable diagnostic for a status code. Useful on both sides of the
/// bridge; avoids duplicating the table in Swift and Kotlin.
[[nodiscard]] std::string describeStatus(EngineStatus status);

} // namespace scribatic::core
