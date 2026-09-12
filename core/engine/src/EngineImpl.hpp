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
#include <deque>
#include <mutex>

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

    // TODO(backend): whisper_context* / llama_context* / sqlite3* handles land
    // here once `make setup-all` has vendored the submodules.
};

} // namespace scribatic::core
