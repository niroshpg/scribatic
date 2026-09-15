// =============================================================================
//  EngineImpl.hpp — Concrete engine. Internal header: never exposed to Swift or
//  JNI, and deliberately absent from the module map so backend types (ggml
//  contexts, llama_context, sqlite3*) can never leak across the bridge.
// =============================================================================
#pragma once

#include "scribatic/core/AudioRingBuffer.hpp"
#include "scribatic/core/EngineInterface.hpp"
#include "scribatic/core/ModelResidency.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct whisper_context;

namespace scribatic::core {

class EngineImpl final : public EngineInterface {
public:
    explicit EngineImpl(EngineConfig config);
    ~EngineImpl() override;

    EngineStatus warmUp() noexcept override;
    void         hibernate() noexcept override;
    EngineState  state() const noexcept override;

    EngineStatus pushAudio(const float* pcmFrames, std::size_t frameCount) noexcept override;
    EngineStatus runTranscriptionPass() noexcept override;
    EngineStatus flush() noexcept override;

    std::vector<TranscriptSegment> drainSegments() override;
    std::string                    summarize(const std::string& transcript) override;

    EngineStatus              indexNote(std::int64_t noteId, const std::string& text) override;
    std::vector<RetrievalHit> search(const std::string& query, std::int32_t topK) override;

    void requestCancel() noexcept override;

    // Reference count driven by Swift ARC / Kotlin handle ownership.
    void retain() noexcept { refCount_.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept;

private:
    /// 16 kHz mono; 30 s of headroom matches the whisper encoder window.
    static constexpr std::size_t kSampleRate     = 16000;
    static constexpr std::size_t kRingCapacity   = kSampleRate * 30;
    static constexpr std::size_t kEmbeddingDims  = 384;

    /// The UI polls a pass roughly every 400 ms. Handing whisper 400 ms of
    /// audio at a time would be both wasteful and inaccurate — the model has
    /// almost no context to work with and re-runs its encoder for a fraction of
    /// a word. Audio is accumulated until there is a window worth decoding.
    static constexpr std::size_t kWindowSamples  = kSampleRate * 5;

    /// A final flush still needs enough audio to be worth a pass; below this it
    /// is noise or a stray syllable.
    static constexpr std::size_t kMinFlushSamples = kSampleRate / 2;

    /// Runs whisper over `window_`, appending finalised segments. Caller must
    /// not hold segmentMutex_.
    EngineStatus decodeWindow(bool flushing) noexcept;

    EngineConfig    config_;
    AudioRingBuffer ring_{kRingCapacity};
    ModelResidency  whisperWeights_;
    ModelResidency  llamaWeights_;

    std::atomic<EngineState> state_{EngineState::Idle};
    std::atomic<bool>        cancelRequested_{false};
    std::atomic<int>         refCount_{1};

    // Scratch PCM staging buffer: preallocated in the constructor so the
    // inference pass performs zero heap traffic per iteration.
    std::vector<float> scratch_;

    std::mutex                    segmentMutex_;
    std::deque<TranscriptSegment> pendingSegments_;

    /// Audio waiting for a decode, and how many samples have already been
    /// decoded before it. whisper reports timestamps relative to the window it
    /// was given, so the offset is what makes them absolute for the transcript.
    std::vector<float> window_;
    std::int64_t       decodedSamples_ = 0;

    /// Owned whisper handle. Declared as an opaque pointer so this header pulls
    /// in no backend type even though it is already private to the library.
    whisper_context* whisper_ = nullptr;

    // TODO(backend): llama_context* / sqlite3* handles land here next.
};

} // namespace scribatic::core
